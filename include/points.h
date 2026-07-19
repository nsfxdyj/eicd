/*
 * points.h - point (tag) table: definitions, decoding and runtime state.
 *
 * A point maps one Modbus datum to a named engineering value. The table
 * is loaded once at startup from a CSV file (see etc/points.csv.example)
 * and shared between the southbound worker threads (producers of values)
 * and the northbound reporting path (consumer). All runtime fields are
 * guarded by the table mutex; definition fields are read-only after load.
 *
 * Reporting model: report-by-exception with a deadband, plus an optional
 * periodic integrity report.
 */
#ifndef EICD_POINTS_H
#define EICD_POINTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "defs.h"
#include "config.h"

/* Modbus function codes supported by the polling engine */
typedef enum {
    MB_FC_COIL     = 1,             /* read coils                      */
    MB_FC_DISCRETE = 2,             /* read discrete inputs            */
    MB_FC_HOLDING  = 3,             /* read holding registers          */
    MB_FC_INPUT    = 4              /* read input registers            */
} mb_func_e;

/* wire data types; bit maps to coil/discrete, the rest to registers */
typedef enum {
    DT_BIT = 0,
    DT_U16, DT_S16,
    DT_U32, DT_S32,
    DT_F32
} dtype_e;

/*
 * 32-bit word/byte order on the wire. Modbus registers are big-endian
 * on the wire, but many PLCs swap words (CDAB) or bytes within words.
 * Letters denote the byte significance of the assembled value: A is the
 * most significant byte.
 */
typedef enum {
    WO_ABCD = 0,                    /* standard big-endian             */
    WO_BADC,                        /* byte-swapped within each word   */
    WO_CDAB,                        /* word-swapped                    */
    WO_DCBA                         /* full little-endian              */
} word_order_e;

typedef enum {
    QUALITY_GOOD = 0,
    QUALITY_UNCERTAIN,
    QUALITY_BAD
} quality_e;

typedef struct {
    /* ---- static definition (immutable after load) ---- */
    char         tag[EICD_TAG_MAX];
    char         device[EICD_NAME_MAX];
    mb_func_e    func;
    uint16_t     addr;              /* zero-based protocol address      */
    dtype_e      dtype;
    word_order_e worder;
    double       scale;             /* eng = raw * scale + offset       */
    double       offset;
    double       deadband;          /* report-by-exception threshold    */
    char         unit[EICD_UNIT_MAX];

    /* ---- runtime state (guarded by point_table_t.mtx) ---- */
    double       value;             /* engineering value                */
    uint16_t     raw[2];            /* last raw register image          */
    quality_e    quality;
    uint64_t     updated_ms;        /* last successful decode (epoch)   */
    uint64_t     reported_ms;
    double       reported_value;
    quality_e    reported_quality;
    bool         reported_once;
} point_t;

typedef struct {
    point_t         items[EICD_MAX_POINTS];
    size_t          count;
    pthread_mutex_t mtx;            /* guards runtime fields of items[] */
} point_table_t;

void        points_init(point_table_t *t);
int         points_load_csv(point_table_t *t, const char *path,
                            const eicd_config_t *cfg);
point_t    *points_find(point_table_t *t, const char *tag);

/* decoding / runtime update - caller holds the table mutex */
size_t      point_reg_span(const point_t *p);
void        point_apply_regs(point_t *p, const uint16_t *regs, size_t nregs,
                             uint64_t now_ms);
void        point_apply_bit(point_t *p, bool bit, uint64_t now_ms);
bool        point_should_report(const point_t *p, uint64_t now_ms,
                                uint32_t integrity_ms);
void        point_mark_reported(point_t *p, uint64_t now_ms);

/* string <-> enum helpers (parsing returns false/0 on failure) */
bool        mb_func_from_str(const char *s, mb_func_e *out);
bool        dtype_from_str(const char *s, dtype_e *out);
bool        word_order_from_str(const char *s, word_order_e *out);
const char *mb_func_str(mb_func_e f);
const char *dtype_str(dtype_e t);
const char *quality_str(quality_e q);

#endif /* EICD_POINTS_H */
