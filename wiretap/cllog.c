/* cllog.c
 *
 * Wiretap Library
 * Copyright (c) 1998 by Gilbert Ramirez <gram@alumni.rice.edu>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Reads log files from CLX000 CAN loggers from CSS Electronics:
 *
 *    https://canlogger.csselectronics.com/clx000-docs/cl1000/log/index.html
 *    https://canlogger.csselectronics.com/clx000-docs/cl2000/log/index.html
 *
 * Based on the cCLLog.c, cCLLog.h, and wtap-cllog.c source files from
 * the WS_v2.4-Plugin_v7.1.zip version of the CSS Electronics plugin at
 *
 *    https://canlogger.csselectronics.com/downloads.php?q=wireshark
 *
 * with the files combined into one source file, modernized to
 * fit into an up-to-date version of Wireshark, and cleaned up
 * not to, for example, do seeks by rewinding and reading to
 * get to the seek target.
 *
 * It could probably use some further cleanup.
 */

#include "config.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "wtap-int.h"
#include "file_wrappers.h"

/***********************************************************************************************************************
 * Public definitions
 **********************************************************************************************************************/
#define MAX_LOG_LINE_FIELDS 7 /*( seqNo, timestamp, lost, SE, ID, length, data) */
/***********************************************************************************************************************
 * Public type declarations
 **********************************************************************************************************************/
/* Time stamp structure type (sec since start + ms resolution) */
typedef struct { time_t epoch; uint16_t ms; } cCLLog_timeStamp_t;

 /* Messasge type */
typedef enum
{
    msg_rx_standard_e = 0,
    msg_rx_extended_e = 1,
    msg_tx_standard_e = 7,
    msg_tx_extended_e = 8,
} cCLLog_messageType_t;

/* Typedef CAN-bus message type */
typedef struct
{
    cCLLog_timeStamp_t timestamp;
    uint8_t lost;
    cCLLog_messageType_t msgType;
    uint32_t id;
    uint8_t length;
    uint8_t data[ 8 ];
} cCLLog_message_t;

/* Silent-mode*/
typedef enum { silent_disabled_e = 0, silent_enabled_e } cCLLog_silentMode_t;

/* Cyclic-mode*/
typedef enum { cyclic_disabled_e = 0, cyclic_enabled_e } cCLLog_cyclicMode_t;

/* Logger type */
typedef enum { type_CL1000_e = 0, type_CL2000_e, type_CL3000_e } cCLLog_loggerType_t;

typedef char * (*CLLog_gets_t)(char *s, int size, void *stream);
typedef int (*CLLog_rewind_t)(void *stream);

typedef struct cLLog_private cCLLog_logFileInfo_t;

/* Type used to parse a field in a log line */
typedef bool (*parseFieldFunc_t)(cCLLog_logFileInfo_t *pInfo, char *pField, cCLLog_message_t *pLogEntry, int *err, char **err_info);

/* Log file information */
struct cLLog_private
{
    uint32_t firstLogRow;
    cCLLog_loggerType_t loggerType;
    char hwrev[5];
    char fwrev[5];
    char id[20];
    uint32_t sessionNo;
    uint32_t splitNo;
    cCLLog_timeStamp_t logStartTime;
    char logStartTimeString[ 20 ];
    char separator;
    uint8_t timeFormat;
    char timeSeparator;
    char timeSeparatorMs;
    char dateSeparator;
    char dateAndTimeSeparator;
    uint32_t bitRate;
    cCLLog_silentMode_t silentMode;
    cCLLog_cyclicMode_t cyclicMode;

    parseFieldFunc_t parseFieldFunc[ MAX_LOG_LINE_FIELDS ];

    /* First log time stamp as relative offset */
    cCLLog_timeStamp_t firstTimeStampAbs;
};

/***********************************************************************************************************************
 * Private definitions
 **********************************************************************************************************************/
#define HEADER_LINE_PARSE_MAPPING_LENGTH ( sizeof( headerLineParseMapping ) / sizeof( headerLineParseMapping[ 0 ] ) )
#define MAX_LOG_LINE_LENGTH 200
#define TIME_STAMP_STRING_MAX_LENGTH ( sizeof( "YYYY/MM/DDThh:mm:ss.kkk" ) )
#define TIME_STAMP_STRING_STRIPPED_MAX_LENGTH ( sizeof( "YYYYMMDDhhmmsskkk" ) )

/***********************************************************************************************************************
 * Private type definitions
 **********************************************************************************************************************/
/* Function type to parse a single log file line */
typedef void( *parseFunc_t )( cCLLog_logFileInfo_t *pInfo, char *pLine );

/* Structure of the header parse mapping. A match string is paired with a parse function */
typedef struct
{
    const char *pMatchString;
    parseFunc_t parseFunc;
} headerLineParseMapping_t;

/***********************************************************************************************************************
 * Private function declarations
 **********************************************************************************************************************/
static bool parseColumnHeaderFields( cCLLog_logFileInfo_t *pInfo, char *pColLine );
static uint8_t stripTimeStamp( const cCLLog_logFileInfo_t *pInfo, char *pTimeStampString );

/* Parse header lines functions */
static void parseLogFileHeaderLine_type( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_fwrev( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_hwrev( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_id( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_sessionNo( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_splitNo( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_time( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_valueSeparator( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_timeFormat( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_timeSeparator( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_timeSeparatorMs( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_dateSeparator( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_timeAndDateSeparator( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_bitRate( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_silentMode( cCLLog_logFileInfo_t *pInfo, char *pLine );
static void parseLogFileHeaderLine_cyclicMode( cCLLog_logFileInfo_t *pInfo, char *pLine );
/***********************************************************************************************************************
 * Private variable definitions
 **********************************************************************************************************************/

/* Array of header line match strings and associated parse functions */
static const headerLineParseMapping_t headerLineParseMapping[] =
{
    { .pMatchString = "Logger type: ", .parseFunc = parseLogFileHeaderLine_type},
    { .pMatchString = "HW rev: ", .parseFunc = parseLogFileHeaderLine_hwrev },
    { .pMatchString = "FW rev: ", .parseFunc = parseLogFileHeaderLine_fwrev },
    { .pMatchString = "Logger ID: ", .parseFunc = parseLogFileHeaderLine_id},
    { .pMatchString = "Session No.: ", .parseFunc = parseLogFileHeaderLine_sessionNo},
    { .pMatchString = "Split No.: ", .parseFunc = parseLogFileHeaderLine_splitNo},
    { .pMatchString = "Time: ", .parseFunc = parseLogFileHeaderLine_time},
    { .pMatchString = "Value separator: ", .parseFunc = parseLogFileHeaderLine_valueSeparator},
    { .pMatchString = "Time format: ", .parseFunc = parseLogFileHeaderLine_timeFormat},
    { .pMatchString = "Time separator: ", .parseFunc = parseLogFileHeaderLine_timeSeparator},
    { .pMatchString = "Time separator ms: ", .parseFunc = parseLogFileHeaderLine_timeSeparatorMs},
    { .pMatchString = "Date separator: ", .parseFunc = parseLogFileHeaderLine_dateSeparator},
    { .pMatchString = "Time and date separator: ", .parseFunc = parseLogFileHeaderLine_timeAndDateSeparator},
    { .pMatchString = "Bit-rate: ", .parseFunc = parseLogFileHeaderLine_bitRate},
    { .pMatchString = "Silent mode: ", .parseFunc = parseLogFileHeaderLine_silentMode},
    { .pMatchString = "Cyclic mode: ", .parseFunc = parseLogFileHeaderLine_cyclicMode},
};

/*
 * Do a string copy to a buffer of a specified length.
 * If the string will fit, return true.
 * If the string won't fit, return false.
 */
static bool
checked_strcpy(char *dest, size_t destlen, char *src)
{
    size_t srclen;

    srclen = strlen(src) + 1; // count the trailing '\0'
    if (srclen > destlen)
        return false;
    memcpy(dest, src, srclen);
    return true;
}

/* TODO: Does not support separators set to numbers (will remove part of the time stamp also */
/* TODO: Does not support time stamps without ms, as given in the header */
/* TODO: Alot of copying slows down the parsing */
static bool parseFieldTS(cCLLog_logFileInfo_t *pInfo, char *pField, cCLLog_message_t *pLogEntry, int *err, char **err_info)
{
    struct tm tm;
    int ms;

    /* Copy the string to not modify the original */
    char timeStampCopy[TIME_STAMP_STRING_MAX_LENGTH];
    if (!checked_strcpy(timeStampCopy, sizeof timeStampCopy, pField))
    {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = g_strdup("cllog: time stamp too long");
        return false;
    }

    /* Copy the header time stamp string to not modify the original */
    char timeStampHeaderCopy[TIME_STAMP_STRING_MAX_LENGTH];
    if (!checked_strcpy(timeStampHeaderCopy, sizeof timeStampHeaderCopy, pInfo->logStartTimeString))
    {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = g_strdup("cllog: header time stamp too long");
        return false;
    }

    /* Strip the delimiters from the time strings */
    uint8_t msgTimeStrippedLen = stripTimeStamp(pInfo, timeStampCopy);
    uint8_t headerTimeStrippedLen = stripTimeStamp(pInfo, timeStampHeaderCopy);

    /* Set time string (YYYYMMDDhhmmsskkk) to the epoch */
    char timeStampStringFull[TIME_STAMP_STRING_STRIPPED_MAX_LENGTH] = "19700101000000000";

    /* Copy the header time to the template */
    memcpy(timeStampStringFull, timeStampHeaderCopy, headerTimeStrippedLen);

    /* Copy the stripped timestamp into the full template */
    memcpy(&timeStampStringFull[TIME_STAMP_STRING_STRIPPED_MAX_LENGTH - 1 - msgTimeStrippedLen], timeStampCopy, msgTimeStrippedLen);
    timeStampStringFull[TIME_STAMP_STRING_STRIPPED_MAX_LENGTH - 1] = '\0';

    memset(&tm, 0, sizeof tm);

    /* YYYYMMDDThhmmss */
    sscanf(timeStampStringFull, "%4u%2u%2u%2u%2u%2u%3u",
            &tm.tm_year,
            &tm.tm_mon,
            &tm.tm_mday,
            &tm.tm_hour,
            &tm.tm_min,
            &tm.tm_sec,
            &ms
            );
    tm.tm_mon -= 1;
    tm.tm_year -= 1900;

    /* To Epoch (mktime converts to epoch from local (!!!) timezone) */
    pLogEntry->timestamp.epoch = mktime(&tm);
    pLogEntry->timestamp.ms = ms;

    /* Is first time stamp ? */
    if (pInfo->firstTimeStampAbs.epoch == 0 && pInfo->firstTimeStampAbs.ms == 0)
    {
        pInfo->firstTimeStampAbs.epoch = pLogEntry->timestamp.epoch;
        pInfo->firstTimeStampAbs.ms = pLogEntry->timestamp.ms;
    }

    return true;
}

static bool parseFieldLost(cCLLog_logFileInfo_t *pInfo _U_, char *pField, cCLLog_message_t *pLogEntry, int *err _U_, char **err_info _U_)
{
    int lost = pLogEntry->lost;

    sscanf(pField, "%i", &lost);
    pLogEntry->lost = lost;
    return true;
}

static bool parseFieldMsgType(cCLLog_logFileInfo_t *pInfo _U_, char *pField, cCLLog_message_t *pLogEntry, int *err, char **err_info)
{
    switch (pField[0])
    {
        case '0':
            pLogEntry->msgType = msg_rx_standard_e;
            return true;
        case '1':
            pLogEntry->msgType = msg_rx_extended_e;
            return true;
        case '8':
            pLogEntry->msgType = msg_tx_standard_e;
            return true;
        case '9':
            pLogEntry->msgType = msg_tx_extended_e;
            return true;
        default:
            *err = WTAP_ERR_BAD_FILE;
            *err_info = g_strdup("cllog: unknown message type");
            return false;
    }
}

static bool parseFieldID(cCLLog_logFileInfo_t *pInfo _U_, char *pField, cCLLog_message_t *pLogEntry, int *err _U_, char **err_info _U_)
{
    sscanf(pField, "%x", &pLogEntry->id);
    return true;
}

static bool parseFieldLength(cCLLog_logFileInfo_t *pInfo _U_, char *pField, cCLLog_message_t *pLogEntry, int *err _U_, char **err_info _U_)
{
    int length = pLogEntry->length;

    sscanf(pField, "%i", &length);
    pLogEntry->length = length;
    return true;
}

static bool parseFieldData(cCLLog_logFileInfo_t *pInfo _U_, char *pField, cCLLog_message_t *pLogEntry, int *err _U_, char **err_info _U_)
{
    char *pFieldStart = pField;

    /* Set data length in case length field is not set explicitly in the log file */
    pLogEntry->length = 0;

    /* Loop all data bytes */
    for (unsigned int dataByte = 0; dataByte < 8; dataByte++)
    {
        unsigned int data = pLogEntry->data[dataByte];

        if (*pFieldStart == '\n' || *pFieldStart == '\r')
        {
            break;
        }

        sscanf(pFieldStart, "%2x", &data);
        pLogEntry->data[dataByte] = data;

        /* Move on byte (two chars) forward */
        pFieldStart += 2;

        pLogEntry->length++;
    }
    return true;
}

static bool parseLogLine(cCLLog_logFileInfo_t *pInfo, char *pLine, cCLLog_message_t *pLogEntry, int *err, char **err_info)
{
    char *pFieldStart = pLine;

    /* Loop all fields in log line */
    for (unsigned int fieldNo = 0, finalField = 0; fieldNo < MAX_LOG_LINE_FIELDS && finalField == 0; fieldNo++)
    {
        /* Find field end by separator */
        char *pFieldEnd = strchr(pFieldStart, pInfo->separator);

        /* If final field, then EOL marks the end of the field */
        if (pFieldEnd == NULL)
        {
            pFieldEnd = strchr(pFieldStart, '\n');
            finalField = 1;
        }

        /* Replace separator with string termination */
        *pFieldEnd = '\0';

        /* Is parse function assigned to field? */
        if (pInfo->parseFieldFunc[fieldNo] != NULL)
        {
            /* Parse field */
            if (!pInfo->parseFieldFunc[fieldNo](pInfo, pFieldStart, pLogEntry, err, err_info))
            {
                return false;
            }
        }

        /* Skip over the separator */
        pFieldStart = pFieldEnd + 1;
    }
    return true;
}

/***********************************************************************************************************************
 * parseColumnHeaderFields
 *
 * Parse the column fields and determine which fields are present and the position of the fields
 *
 * @param[ in ]         pInfo           Pointer to the CLLog object
 * @param[ in ]         pColLine        The column line
 **********************************************************************************************************************/
static bool parseColumnHeaderFields( cCLLog_logFileInfo_t *pInfo, char *pColLine )
{
    bool resultFlag = false;

    /* Initialise field start */
    char *pFieldStart = pColLine;

    /* Loop all fields in line */
    for ( uint8_t fieldNo = 0, finalField = 0 ; fieldNo < MAX_LOG_LINE_FIELDS && finalField == 0 ; fieldNo++ )
    {
        /* Find field end */
        char *pFieldEnd = strchr( pFieldStart, pInfo->separator );

        /* If final field, then EOL marks the end of the field */
        if( pFieldEnd == NULL )
        {
            pFieldEnd = strchr( pFieldStart, '\n' );
            finalField = 1;
        }

        /* Replace separator with string termination */
        *pFieldEnd = '\0';

        /* Set field number */
        if( strcmp( pFieldStart, "Timestamp" ) == 0 )  { pInfo->parseFieldFunc[ fieldNo ] = parseFieldTS; resultFlag = true; }
        if( strcmp( pFieldStart, "Lost" ) == 0 )       { pInfo->parseFieldFunc[ fieldNo ] = parseFieldLost; resultFlag = true; }
        if( strcmp( pFieldStart, "Type" ) == 0 )       { pInfo->parseFieldFunc[ fieldNo ] = parseFieldMsgType; resultFlag = true; }
        if( strcmp( pFieldStart, "ID" ) == 0 )         { pInfo->parseFieldFunc[ fieldNo ] = parseFieldID; resultFlag = true; }
        if( strcmp( pFieldStart, "Length" ) == 0 )     { pInfo->parseFieldFunc[ fieldNo ] = parseFieldLength; resultFlag = true; }
        if( strcmp( pFieldStart, "Data" ) == 0 )       { pInfo->parseFieldFunc[ fieldNo ] = parseFieldData; resultFlag = true; }

        /* Set start of next field to end of privious + 1 */
        pFieldStart = pFieldEnd + 1;
    }

    return resultFlag;
}

/***********************************************************************************************************************
 * stripTimeStamp
 *
 * Strips a time stamp string for any delimiters
 **********************************************************************************************************************/
static uint8_t stripTimeStamp( const cCLLog_logFileInfo_t *pInfo, char *pTimeStampString )
{
    uint8_t strippedLength = 0U;

    /* Char by char, strip the delimiters from the time stamp string */
    uint8_t timeStampStringLen = (uint8_t) strlen( pTimeStampString );
    for( uint8_t i = 0U ; i < timeStampStringLen ; i++ )
    {
        /* Get char */
        char charTmp = pTimeStampString[ i ];

        /* If delimiter, skip */
        if( charTmp == pInfo->separator ){ continue; }
        if( charTmp == pInfo->timeSeparator ){ continue; }
        if( charTmp == pInfo->timeSeparatorMs ){ continue; }
        if( charTmp == pInfo->dateSeparator ){ continue; }
        if( charTmp == pInfo->dateAndTimeSeparator ){ continue; }

        /* Not a delimiter, keep char */
        pTimeStampString[ strippedLength++ ] = charTmp;
    }
    pTimeStampString[ strippedLength ] = '\0';

    return strippedLength;
}
/***********************************************************************************************************************
 * parseLogFileHeaderLine_X
 *
 * Parse log file header line functions
 *
 * @param[ in ]         pLine               Header line
 **********************************************************************************************************************/
static char* getFieldValue( char *pLine )
{
    /* Set start pointer to fist byte in value */
    char *pFieldStart = strstr( pLine, ": ") + 2;

    /* Replace any newline chars with end of line */
    for( char *pChar = pFieldStart ; ; pChar++ )
    {
        if( ( *pChar == '\n' ) || ( *pChar == '\r' ) || ( *pChar == '\0' ) )
        {
            *pChar = '\0';
            break;
        }
    }
    return pFieldStart;
}

static char parseSeparator( char *pFieldValue )
{
    char separator = '\0';
    /* Separator field is if set e.g. ";" - that is 3 chars. Else it is "" */
    if( strlen( pFieldValue ) == 3)
    {
        sscanf( pFieldValue, "\"%c\"", &separator );
    }
    return separator;
}

static void parseHeaderTime( const char *pTimeStampString, cCLLog_timeStamp_t *pTs )
{
    struct tm tm;
    memset( &tm, 0, sizeof( tm ) );

    /* YYYYMMDDThhmmss */
    sscanf( pTimeStampString,
            "%4u%2u%2uT%2u%2u%2u",
            &tm.tm_year,
            &tm.tm_mon,
            &tm.tm_mday,
            &tm.tm_hour,
            &tm.tm_min,
            &tm.tm_sec );
    tm.tm_mon -= 1;
    tm.tm_year -= 1900;

    /* To Epoch ( mktime converts to epoch from local (!!!) timezone )*/
    pTs->epoch = mktime( &tm );
    pTs->ms = 0;
}

static void parseLogFileHeaderLine_type( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    if( strcmp( getFieldValue( pLine ), "CANLogger1000" ) == 0 ){ pInfo->loggerType = type_CL1000_e; }
    if( strcmp( getFieldValue( pLine ), "CANLogger2000" ) == 0 ){ pInfo->loggerType = type_CL2000_e; }
    if( strcmp( getFieldValue( pLine ), "CANLogger3000" ) == 0 ){ pInfo->loggerType = type_CL3000_e; }
}

static void parseLogFileHeaderLine_hwrev( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    sscanf( getFieldValue( pLine ), "%s", pInfo->hwrev );
}

static void parseLogFileHeaderLine_fwrev( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    sscanf( getFieldValue( pLine ), "%s", pInfo->fwrev );
}

static void parseLogFileHeaderLine_id( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    sscanf( getFieldValue( pLine ), "%s", pInfo->id );
}

static void parseLogFileHeaderLine_sessionNo( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    sscanf( getFieldValue( pLine ), "%i", &pInfo->sessionNo );
}

static void parseLogFileHeaderLine_splitNo( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    sscanf( getFieldValue( pLine ), "%i", &pInfo->splitNo );
}

static void parseLogFileHeaderLine_time( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    const char *pFieldStart = getFieldValue( pLine );
    parseHeaderTime( pFieldStart, &pInfo->logStartTime );
    memcpy( pInfo->logStartTimeString, pFieldStart, strlen( pFieldStart ) );
}

static void parseLogFileHeaderLine_valueSeparator( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    pInfo->separator = parseSeparator( getFieldValue( pLine ) );
}

static void parseLogFileHeaderLine_timeFormat( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    int formatTmp = 0;
    sscanf( getFieldValue( pLine ), "%i", &formatTmp );
    pInfo->timeFormat = (uint8_t)formatTmp;
}

static void parseLogFileHeaderLine_timeSeparator( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    pInfo->timeSeparator = parseSeparator( getFieldValue( pLine ) );
}

static void parseLogFileHeaderLine_timeSeparatorMs( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    pInfo->timeSeparatorMs = parseSeparator( getFieldValue( pLine ) );
}

static void parseLogFileHeaderLine_dateSeparator( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    pInfo->dateSeparator = parseSeparator( getFieldValue( pLine ) );
}

static void parseLogFileHeaderLine_timeAndDateSeparator( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    pInfo->dateAndTimeSeparator = parseSeparator( getFieldValue( pLine ) );
}

static void parseLogFileHeaderLine_bitRate( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    sscanf( getFieldValue( pLine ), "%i", &pInfo->bitRate );
}

static void parseLogFileHeaderLine_silentMode( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    if( strcmp( getFieldValue( pLine ), "true" ) == 0 ){ pInfo->silentMode = silent_enabled_e; }
    if( strcmp( getFieldValue( pLine ), "false" ) == 0 ){ pInfo->silentMode = silent_disabled_e; }
}

static void parseLogFileHeaderLine_cyclicMode( cCLLog_logFileInfo_t *pInfo, char *pLine )
{
    if( strcmp( getFieldValue( pLine ), "true" ) == 0 ){ pInfo->cyclicMode = cyclic_enabled_e; }
    if( strcmp( getFieldValue( pLine ), "false" ) == 0 ){ pInfo->cyclicMode = cyclic_disabled_e; }
}

/*

         c:\development\wireshark\plugins\wimaxmacphy\cCLLog.c(248): warning C4
       477: 'sscanf' : format string '%i' requires an argument of type 'int *',
        but variadic argument 1 has type 'uint8_t *'
         c:\development\wireshark\plugins\wimaxmacphy\cCLLog.c(274): warning C4
       477: 'sscanf' : format string '%i' requires an argument of type 'int *',
        but variadic argument 1 has type 'uint8_t *'
         c:\development\wireshark\plugins\wimaxmacphy\cCLLog.c(288): warning C4
       477: 'sscanf' : format string '%2x' requires an argument of type 'unsign
       ed int *', but variadic argument 1 has type 'uint8_t *


*/

#include "cllog.h"

static int cllog_file_type_subtype = -1;

#define CAN_EFF_MASK 0x1FFFFFFF /* extended frame format (EFF) */
#define CAN_SFF_MASK 0x000007FF /* standard frame format (SFF) */

static gboolean
cllog_read_common(wtap *wth, FILE_T fh, wtap_rec *rec, Buffer *buf, int *err, gchar **err_info _U_)
{
    cCLLog_logFileInfo_t *clLog = (cCLLog_logFileInfo_t *) wth->priv;
    char line[MAX_LOG_LINE_LENGTH];
    cCLLog_message_t logEntry;
    guint8 *can_data;

    /* Read a line */
    if (file_gets(line, sizeof(line), fh) == NULL)
    {
        /* EOF or error. */
        *err = file_error(wth->fh, err_info);
        return FALSE;
    }

    /* Default the log entry structure */
    memset(&logEntry, 0, sizeof(logEntry));

    /* Parse the line */
    if (!parseLogLine(clLog, line, &logEntry, err, err_info))
    {
        return FALSE;
    }

    rec->rec_type = REC_TYPE_PACKET;
    rec->block = wtap_block_create(WTAP_BLOCK_PACKET);
    rec->presence_flags = WTAP_HAS_TS;

    rec->ts.secs = logEntry.timestamp.epoch;
    rec->ts.nsecs = logEntry.timestamp.ms * 1000U * 1000U;

    rec->rec_header.packet_header.caplen = 8 + logEntry.length;
    rec->rec_header.packet_header.len = 8 + logEntry.length;

    if (logEntry.msgType == msg_tx_standard_e || logEntry.msgType == msg_tx_extended_e)
    {
        wtap_block_add_uint32_option(rec->block, OPT_PKT_FLAGS, PACK_FLAGS_DIRECTION_OUTBOUND);
    }
    else if (logEntry.msgType == msg_rx_standard_e || logEntry.msgType == msg_rx_extended_e)
    {
        wtap_block_add_uint32_option(rec->block, OPT_PKT_FLAGS, PACK_FLAGS_DIRECTION_INBOUND);
    }

    ws_buffer_assure_space(buf, rec->rec_header.packet_header.caplen);
    can_data = ws_buffer_start_ptr(buf);

    can_data[0] = (logEntry.id >> 24);
    can_data[1] = (logEntry.id >> 16);
    can_data[2] = (logEntry.id >>  8);
    can_data[3] = (logEntry.id >>  0);
    can_data[4] = logEntry.length;
    can_data[5] = 0;
    can_data[6] = 0;
    can_data[7] = 0;

    if (logEntry.msgType == msg_tx_extended_e || logEntry.msgType == msg_rx_extended_e || (logEntry.id & CAN_EFF_MASK) > CAN_SFF_MASK)
        can_data[0] |= 0x80;

    memcpy(&can_data[8], logEntry.data, logEntry.length);
    return TRUE;
}

static gboolean
cllog_read(wtap *wth, wtap_rec *rec, Buffer *buf, int *err, gchar **err_info, gint64 *data_offset)
{
    *data_offset = file_tell(wth->fh);

    return cllog_read_common(wth, wth->fh, rec, buf, err, err_info);
}

static gboolean
cllog_seek_read(wtap *wth, gint64 seek_off, wtap_rec *rec, Buffer *buf, int *err, gchar **err_info)
{
    if (file_seek(wth->random_fh, seek_off, SEEK_SET, err) == -1)
        return FALSE;

    return cllog_read_common(wth, wth->random_fh, rec, buf, err, err_info);
}

wtap_open_return_val
cllog_open(wtap *wth, int *err, gchar **err_info _U_)
{
    cCLLog_logFileInfo_t *clLog;
    char line[ MAX_LOG_LINE_LENGTH ];

    clLog = g_new0(cCLLog_logFileInfo_t, 1);

    /* Initialize the header information */
    clLog->loggerType = 0;
    clLog->hwrev[0] = '\0';
    clLog->fwrev[0] = '\0';
    clLog->id[0] = '\0';
    clLog->sessionNo = 0;
    clLog->splitNo = 0;
    clLog->logStartTime.epoch = 0;
    clLog->logStartTime.ms = 0;
    clLog->logStartTimeString[0] = '\0';
    clLog->separator = '\0';
    clLog->timeFormat = 0;
    clLog->timeSeparator = '\0';
    clLog->timeSeparatorMs = '\0';
    clLog->dateSeparator = '\0';
    clLog->dateAndTimeSeparator = '\0';
    clLog->bitRate = 0;
    clLog->silentMode = 0;
    clLog->cyclicMode = 0;

    /* Set parse function pointers */
    memset(clLog->parseFieldFunc, 0, sizeof( clLog->parseFieldFunc));

    /*
     * We're at the beginning of the file; read each line and
     * parse it.
     */
    while (file_gets(line, sizeof(line), wth->fh) != NULL)
    {
        if (*err != 0)
        {
            if (*err != WTAP_ERR_SHORT_READ)
            {
                /* Incomplete header, so not ours. */
                g_free(clLog);
                return WTAP_OPEN_NOT_MINE;
            }
            else
            {
                /* I/O error. */
                g_free(clLog);
                return WTAP_OPEN_ERROR;
            }
        }

        /* Break on end of header */
        if (line[0] != '#')
        {
            break;
        }

        for (unsigned int i = 0U; i < HEADER_LINE_PARSE_MAPPING_LENGTH; i++)
        {
            const headerLineParseMapping_t *pHeaderMapping = &headerLineParseMapping[ i ];

            if (strstr(line, pHeaderMapping->pMatchString) != NULL &&
                 pHeaderMapping->parseFunc != NULL)
            {
                pHeaderMapping->parseFunc(clLog, line);
            }
        }
    }

    /*
     * We've read the first line after the header, so it's the column
     * header line. Parse it.
     */
    if (!parseColumnHeaderFields(clLog, line))
    {
        g_free(clLog);
        return WTAP_OPEN_NOT_MINE;
    }

    wth->priv = clLog;

    wth->file_type_subtype = cllog_file_type_subtype;
    wth->file_encap = WTAP_ENCAP_SOCKETCAN;
    wth->snapshot_length = 0;

    wth->subtype_read = cllog_read;
    wth->subtype_seek_read = cllog_seek_read;
    wth->file_tsprec = WTAP_TSPREC_MSEC;

    return WTAP_OPEN_MINE;
}

/* Options for packet blocks. */
static const struct supported_option_type packet_block_options_supported[] = {
    { OPT_PKT_FLAGS, ONE_OPTION_SUPPORTED },
};

static const struct supported_block_type cllog_blocks_supported[] = {
    /*
     * We support packet blocks, with only the flags option supported.
     */
    { WTAP_BLOCK_PACKET, MULTIPLE_BLOCKS_SUPPORTED, OPTION_TYPES_SUPPORTED(packet_block_options_supported) }
};

static const struct file_type_subtype_info cllog_info = {
    "CSS Electronics CLX000 CAN log", "cllog", "txt", NULL,
    FALSE, BLOCKS_SUPPORTED(cllog_blocks_supported),
    NULL, NULL, NULL
};

void
register_canlogger(void)
{
    cllog_file_type_subtype = wtap_register_file_type_subtype(&cllog_info);
}