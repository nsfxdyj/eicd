/*
 * defs.h - global constants, limits and shared return codes for eicd.
 *
 * Everything tunable at runtime lives in the configuration file; this
 * header only carries compile-time hard limits and protocol boundaries.
 */
#ifndef EICD_DEFS_H
#define EICD_DEFS_H

#define EICD_VERSION        "1.0.0"
#define EICD_PROGNAME       "eicd"

/* ------------------------------------------------------------------ */
/* hard limits                                                         */
/* ------------------------------------------------------------------ */
#define EICD_MAX_DEVICES        32      /* southbound devices per instance   */
#define EICD_MAX_POINTS         2048    /* tags in the point table           */
#define EICD_NAME_MAX           32      /* device name                       */
#define EICD_HOST_MAX           128     /* hostname / IPv4 / IPv6 literal    */
#define EICD_PATH_MAX_LEN       256     /* filesystem paths from config      */
#define EICD_TOPIC_MAX          160     /* MQTT publish topic                */
#define EICD_TAG_MAX            64      /* point tag                         */
#define EICD_UNIT_MAX           16      /* engineering unit string           */
#define EICD_MQTT_STR_MAX       128     /* client id / username / password   */

/* ------------------------------------------------------------------ */
/* Modbus protocol boundaries (see Modbus Application Protocol v1.1b3) */
/* ------------------------------------------------------------------ */
#define EICD_MB_MAX_READ_REGS   120     /* stay below the spec max of 125    */
#define EICD_MB_MAX_READ_BITS   1968    /* spec max is 2000, keep margin     */

/* ------------------------------------------------------------------ */
/* shared return codes                                                 */
/* ------------------------------------------------------------------ */
#define EICD_OK                 0
#define EICD_ERR                (-1)
#define EICD_ERR_TIMEOUT        (-2)

#endif /* EICD_DEFS_H */
