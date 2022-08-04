/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * TRACE Decoder Module
 * ====================
 *
 * Implementation of ITM/DWT decode according to the specification in Appendix D4
 * of the ARMv7-M Architecture Refrence Manual document available
 * from https://static.docs.arm.com/ddi0403/e/DDI0403E_B_armv7m_arm.pdf
 */

#include <string.h>
#include <assert.h>
#include "msgDecoder.h"
#include "traceDecoder.h"
#include "generics.h"

/* Events from the process of pumping bytes through the TRACE decoder */
enum TRACEDecoderPumpEvent
{
    TRACE_EV_NONE,
    TRACE_EV_UNSYNCED,
    TRACE_EV_SYNCED,
    TRACE_EV_ERROR,
    TRACE_EV_MSG_RXED
};

const char *TRACEprotocolString[] =
{
    TRACEProtocolStringDEF
};

const char *protoStateName[] =
{
    TRACEprotoStateNamesDEF
};

// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Internal routines - the decoder itself
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================


// ====================================================================================================
static void _stateChange( struct TRACEDecoder *i, enum TRACEchanges c )
{
    i->cpu.changeRecord |= ( 1 << c );
}
// ====================================================================================================
static void _ETM35DecoderPumpAction( struct TRACEDecoder *i, uint8_t c, traceDecodeCB cb, genericsReportCB report, void *d )

/* Pump next byte into the protocol decoder */

{
    bool C;                               /* Is address packet continued? */
    bool X = false;                       /* Is there exception information following address */
    int8_t ofs;                           /* Offset for bits in address calculation */
    uint8_t mask;                         /* Mask for bits in address calculation */

    enum TRACEprotoState newState = i->p;
    struct TRACECPUState *cpu = &i->cpu;
    enum TRACEDecoderPumpEvent retVal = TRACE_EV_NONE;


    /* Perform A-Sync accumulation check */
    if ( ( i->asyncCount >= 5 ) && ( c == 0x80 ) )
    {
        if ( report )
        {
            report( V_DEBUG, "A-Sync Accumulation complete" EOL );
        }

        newState = TRACE_IDLE;
    }
    else
    {
        i->asyncCount = c ? 0 : i->asyncCount + 1;

        switch ( i->p )
        {
            // -----------------------------------------------------
            case TRACE_UNSYNCED:
                break;

            // -----------------------------------------------------

            case TRACE_IDLE:

                // *************************************************
                // ************** BRANCH PACKET ********************
                // *************************************************
                if ( c & 0b1 )
                {
                    /* The lowest order 6 bits of address info... */

                    switch ( cpu->addrMode )
                    {
                        case TRACE_ADDRMODE_ARM:
                            i->addrConstruct = ( i->addrConstruct & ~( 0b11111100 ) ) | ( ( c & 0b01111110 ) << 1 );
                            break;

                        case TRACE_ADDRMODE_THUMB:
                            i->addrConstruct = ( i->addrConstruct & ~( 0b01111111 ) ) | ( c & 0b01111110 );
                            break;

                        case TRACE_ADDRMODE_JAZELLE:
                            i->addrConstruct = ( i->addrConstruct & ~( 0b00111111 ) ) | ( ( c & 0b01111110 ) >> 1 );
                            break;
                    }

                    i->byteCount = 1;
                    C = ( c & 0x80 ) != 0;
                    X = false;
                    _stateChange( i, EV_CH_ADDRESS );

                    newState = ( i->usingAltAddrEncode ) ? TRACE_COLLECT_BA_ALT_FORMAT : TRACE_COLLECT_BA_STD_FORMAT;
                    goto terminateAddrByte;
                }

                // *************************************************
                // ************** A-SYNC PACKET ********************
                // *************************************************
                if ( c == 0b00000000 )
                {
                    break;
                }

                // *************************************************
                // ************ CYCLECOUNT PACKET ******************
                // *************************************************
                if ( c == 0b00000100 )
                {
                    if ( report )
                    {
                        report( V_DEBUG, "CYCCNT " EOL );
                    }

                    i->byteCount = 0;
                    i->cycleConstruct = 0;
                    newState = TRACE_GET_CYCLECOUNT;
                    break;
                }

                // *************************************************
                // ************** ISYNC PACKETS ********************
                // *************************************************
                if ( c == 0b00001000 ) /* Normal ISYNC */
                {
                    if ( report )
                    {
                        report( V_DEBUG, "Normal ISYNC " EOL );
                    }

                    /* Collect either the context or the Info Byte next */
                    i->byteCount = 0;
                    i->contextConstruct = 0;
                    newState = i->contextBytes ? TRACE_GET_CONTEXTBYTE : TRACE_GET_INFOBYTE;

                    /* We won't start reporting data until a valid ISYNC has been received */
                    if ( !i->rxedISYNC )
                    {
                        if ( report )
                        {
                            report( V_DEBUG, "Initial ISYNC" );
                        }

                        i->cpu.changeRecord = 0;
                        i->rxedISYNC = true;
                    }

                    break;
                }

                if ( c == 0b01110000 ) /* ISYNC with Cycle Count */
                {
                    if ( report )
                    {
                        report( V_DEBUG, "ISYNC+CYCCNT " EOL );
                    }

                    /* Collect the cycle count next */
                    i->byteCount = 0;
                    i->cycleConstruct = 0;
                    newState = TRACE_GET_ICYCLECOUNT;
                    break;
                }

                // *************************************************
                // ************** TRIGGER PACKET *******************
                // *************************************************
                if ( c == 0b00001100 )
                {
                    if ( report )
                    {
                        report( V_DEBUG, "TRIGGER " EOL );
                    }

                    _stateChange( i, EV_CH_TRIGGER );
                    retVal = TRACE_EV_MSG_RXED;
                    break;
                }

                // *************************************************
                // **************** VMID PACKET ********************
                // *************************************************
                if ( c == 0b00111100 )
                {
                    if ( report )
                    {
                        report( V_DEBUG, "VMID " EOL );
                    }

                    newState = TRACE_GET_VMID;
                    break;
                }

                // *************************************************
                // *********** TIMESTAMP PACKET ********************
                // *************************************************
                if ( ( c & 0b11111011 ) == 0b01000010 )
                {
                    if ( report )
                    {
                        report( V_DEBUG, "TS " EOL );
                    }

                    newState = TRACE_GET_TSTAMP;

                    if ( ( c & ( 1 << 2 ) ) != 0 )
                    {
                        _stateChange( i, EV_CH_CLOCKSPEED );
                    }

                    i->byteCount = 0;
                    break;
                }

                // *************************************************
                // ************** IGNORE PACKET ********************
                // *************************************************
                if ( c == 0b01100110 )
                {
                    if ( report )
                    {
                        report( V_DEBUG, "Ignore Packet" EOL );
                    }

                    break;
                }

                // *************************************************
                // ************ CONTEXTID PACKET *******************
                // *************************************************
                if ( c == 0b01101110 )
                {
                    if ( report )
                    {
                        report( V_DEBUG, "CONTEXTID " EOL );
                    }

                    newState = TRACE_GET_CONTEXTID;
                    cpu->contextID = 0;
                    i->byteCount = 0;
                    break;
                }

                // *************************************************
                // ******** EXCEPTION EXIT PACKET ******************
                // *************************************************
                if ( c == 0b01110110 )
                {
                    if ( report )
                    {
                        report( V_DEBUG, "EXCEPT-EXIT " EOL );
                    }

                    _stateChange( i, EV_CH_EX_EXIT );
                    retVal = TRACE_EV_MSG_RXED;
                    break;
                }

                // *************************************************
                // ******** EXCEPTION ENTRY PACKET *****************
                // *************************************************
                if ( c == 0b01111110 )
                {
                    /* Note this is only used on CPUs with data tracing */
                    if ( report )
                    {
                        report( V_DEBUG, "EXCEPT-ENTRY " EOL );
                    }

                    _stateChange( i, EV_CH_EX_ENTRY );
                    retVal = TRACE_EV_MSG_RXED;
                    break;
                }

                // *************************************************
                // ************** P-HEADER PACKET ******************
                // *************************************************
                if ( ( c & 0b10000001 ) == 0b10000000 )
                {
                    if ( !i->cycleAccurate )
                    {
                        if ( ( c & 0b10000011 ) == 0b10000000 )
                        {
                            /* Format-1 P-header */
                            cpu->eatoms = ( c & 0x3C ) >> 2;
                            cpu->natoms = ( c & ( 1 << 6 ) ) ? 1 : 0;
                            cpu->instCount += cpu->eatoms + cpu->natoms;

                            /* Put a 1 in each element of disposition if was executed */
                            cpu->disposition = ( 1 << cpu->eatoms ) - 1;
                            _stateChange( i, EV_CH_ENATOMS );
                            retVal = TRACE_EV_MSG_RXED;

                            if ( report )
                            {
                                report( V_DEBUG, "PHdr FMT1 (%02x E=%d, N=%d)" EOL, c, cpu->eatoms, cpu->natoms );
                            }

                            break;
                        }

                        if ( ( c & 0b11110011 ) == 0b10000010 )
                        {
                            /* Format-2 P-header */
                            cpu->eatoms = ( ( c & ( 1 << 2 ) ) == 0 ) + ( ( c & ( 1 << 3 ) ) == 0 );
                            cpu->natoms = 2 - cpu->eatoms;

                            cpu->disposition = ( ( c & ( 1 << 3 ) ) == 0 ) |
                                               ( ( ( c & ( 1 << 2 ) ) == 0 ) << 1 );

                            _stateChange( i, EV_CH_ENATOMS );
                            cpu->instCount += cpu->eatoms + cpu->natoms;
                            retVal = TRACE_EV_MSG_RXED;

                            if ( report )
                            {
                                report( V_DEBUG, "PHdr FMT2 (E=%d, N=%d)" EOL, cpu->eatoms, cpu->natoms );
                            }

                            break;
                        }

                        if ( report )
                        {
                            report( V_ERROR, "Unprocessed P-Header (%02X)" EOL, c );
                        }
                    }
                    else
                    {
                        if ( c == 0b10000000 )
                        {
                            /* Format 0 cycle-accurate P-header */
                            cpu->watoms = 1;
                            cpu->instCount += cpu->watoms;
                            cpu->eatoms = cpu->natoms = 0;
                            _stateChange( i, EV_CH_ENATOMS );
                            _stateChange( i, EV_CH_WATOMS );
                            retVal = TRACE_EV_MSG_RXED;

                            if ( report )
                            {
                                report( V_DEBUG, "CA PHdr FMT0 (W=%d)" EOL, cpu->watoms );
                            }

                            break;
                        }

                        if ( ( c & 0b10100011 ) == 0b10000000 )
                        {
                            /* Format 1 cycle-accurate P-header */
                            cpu->eatoms = ( c & 0x1c ) >> 2;
                            cpu->natoms = ( c & 0x40 ) != 0;
                            cpu->watoms = cpu->eatoms + cpu->natoms;
                            cpu->instCount += cpu->watoms;
                            cpu->disposition = ( 1 << cpu->eatoms ) - 1;
                            _stateChange( i, EV_CH_ENATOMS );
                            _stateChange( i, EV_CH_WATOMS );
                            retVal = TRACE_EV_MSG_RXED;

                            if ( report )
                            {
                                report( V_DEBUG, "CA PHdr FMT1 (E=%d, N=%d)" EOL, cpu->eatoms, cpu->natoms );
                            }

                            break;
                        }

                        if ( ( c & 0b11110011 ) == 0b10000010 )
                        {
                            /* Format 2 cycle-accurate P-header */
                            cpu->eatoms = ( ( c & ( 1 << 2 ) ) != 0 ) + ( ( c & ( 1 << 3 ) ) != 0 );
                            cpu->natoms = 2 - cpu->eatoms;
                            cpu->watoms = 1;
                            cpu->instCount += cpu->watoms;
                            cpu->disposition = ( ( c & ( 1 << 3 ) ) != 0 ) | ( ( c & ( 1 << 2 ) ) != 0 );
                            _stateChange( i, EV_CH_ENATOMS );
                            _stateChange( i, EV_CH_WATOMS );
                            retVal = TRACE_EV_MSG_RXED;

                            if ( report )
                            {
                                report( V_DEBUG, "CA PHdr FMT2 (E=%d, N=%d, W=1)" EOL, cpu->eatoms, cpu->natoms );
                            }

                            break;
                        }

                        if ( ( c & 0b10100000 ) == 0b10100000 )
                        {
                            /* Format 3 cycle-accurate P-header */
                            cpu->eatoms = ( c & 0x40 ) != 0;
                            cpu->natoms = 0;
                            cpu->watoms = ( c & 0x1c ) >> 2;
                            cpu->instCount += cpu->watoms;
                            /* Either 1 or 0 eatoms */
                            cpu->disposition = cpu->eatoms;
                            _stateChange( i, EV_CH_ENATOMS );
                            _stateChange( i, EV_CH_WATOMS );
                            retVal = TRACE_EV_MSG_RXED;

                            if ( report )
                            {
                                report( V_DEBUG, "CA PHdr FMT3 (E=%d, N=%d W=%d)" EOL, cpu->eatoms, cpu->natoms, cpu->watoms );
                            }

                            break;
                        }

                        if ( ( c & 0b11111011 ) == 0b10010010 )
                        {
                            /* Format 4 cycle-accurate P-header */
                            cpu->eatoms = ( c & 0x4 ) != 0;
                            cpu->natoms = ( c & 0x4 ) == 0;
                            cpu->watoms = 0;

                            /* Either 1 or 0 eatoms */
                            cpu->disposition = cpu->eatoms;
                            _stateChange( i, EV_CH_ENATOMS );
                            _stateChange( i, EV_CH_WATOMS );
                            retVal = TRACE_EV_MSG_RXED;

                            if ( report )
                            {
                                report( V_DEBUG, "CA PHdr FMT4 (E=%d, N=%d W=%d)" EOL, cpu->eatoms, cpu->natoms, cpu->watoms );
                            }

                            break;
                        }

                        if ( report )
                        {
                            report( V_ERROR, "Unprocessed Cycle-accurate P-Header (%02X)" EOL, c );
                        }
                    }

                    break;
                }

                break;


            // -----------------------------------------------------
            // ADDRESS COLLECTION RELATED ACTIVITIES
            // -----------------------------------------------------

            case TRACE_COLLECT_BA_ALT_FORMAT: /* Collecting a branch address, alt format */
                C = c & 0x80;
                /* This is a proper mess. Mask and collect bits according to address mode in use and */
                /* if it's the last byte of the sequence */
                mask = C ? 0x7f : 0x3f;
                ofs = ( cpu->addrMode == TRACE_ADDRMODE_ARM ) ? 1 : ( cpu->addrMode == TRACE_ADDRMODE_THUMB ) ? 0 : -1;


                i->addrConstruct = ( i->addrConstruct &   ~( mask << ( 7 * i->byteCount + ofs ) ) )
                                   | ( ( c & mask ) << ( 7 * i->byteCount + ofs ) );
                /* There is exception information only if no continuation and bit 6 set */
                X = ( ( !C ) && ( c & 0x40 ) );
                i->byteCount++;
                goto terminateAddrByte;

            // -----------------------------------------------------

            case TRACE_COLLECT_BA_STD_FORMAT: /* Collecting a branch address, standard format */
                /* This will potentially collect too many bits, but that is OK */
                ofs = ( cpu->addrMode == TRACE_ADDRMODE_ARM ) ? 1 : ( cpu->addrMode == TRACE_ADDRMODE_THUMB ) ? 0 : -1;
                i->addrConstruct = ( i->addrConstruct &  ~( 0x7F << ( ( 7 * i->byteCount ) + ofs ) ) ) | ( c & ( 0x7F <<  ( ( 7 * i->byteCount ) + ofs ) ) );
                i->byteCount++;
                C = ( i->byteCount < 5 ) ? c & 0x80 : c & 0x40;
                X = ( i->byteCount == 5 ) && C;
                goto terminateAddrByte;

                // -----------------------------------------------------

                /* For all cases, see if the address is complete, and process if so */
                /* this is a continuation of TRACE_COLLECT_BA_???_FORMAT.             */
            terminateAddrByte:

                /* Check to see if this packet is complete, and encode to return if so */
                if ( ( !C ) || ( i->byteCount == 5 ) )
                {
                    cpu->addr = i->addrConstruct;

                    if ( ( i->byteCount == 5 ) && ( cpu->addrMode == TRACE_ADDRMODE_ARM ) && C )
                    {
                        /* There is (legacy) exception information in here */
                        cpu->exception = ( c >> 4 ) & 0x07;
                        _stateChange( i, EV_CH_EXCEPTION );
                        _stateChange( i, ( ( c & 0x40 ) != 0 ) ? EV_CH_CANCELLED : 0 );
                        newState = TRACE_IDLE;
                        retVal = TRACE_EV_MSG_RXED;

                        if ( report )
                        {
                            report( V_DEBUG, "Branch to %08x with exception %d" EOL, cpu->addr, cpu->exception );
                        }

                        break;
                    }

                    if ( ( !C ) & ( !X ) )
                    {
                        /* This packet is complete, so can return it */
                        newState = TRACE_IDLE;
                        retVal = TRACE_EV_MSG_RXED;

                        if ( report )
                        {
                            report( V_DEBUG, "Branch to %08x" EOL, cpu->addr );
                        }
                    }
                    else
                    {
                        /* This packet also contains exception information, so collect it */
                        i->byteCount = 0; /* Used as a flag of which byte of exception we're collecting */
                        cpu->resume = 0;
                        _stateChange( i, EV_CH_EX_ENTRY );
                        newState = TRACE_COLLECT_EXCEPTION;
                    }
                }

                break;

            // -----------------------------------------------------

            case TRACE_COLLECT_EXCEPTION: /* Collecting exception information */
                if ( i->byteCount == 0 )
                {
                    if ( ( ( c & ( 1 << 0 ) ) != 0 ) != cpu->nonSecure )
                    {
                        cpu->nonSecure = ( ( c & ( 1 << 0 ) ) != 0 );
                        _stateChange( i, EV_CH_SECURE );
                    }

                    cpu->exception = ( c >> 1 ) & 0x0f;
                    _stateChange( i, ( ( c & ( 1 << 5 ) ) != 0 ) ? EV_CH_CANCELLED : 0 );

                    if ( cpu->altISA != ( ( c & ( 1 << 6 ) ) != 0 ) )
                    {
                        cpu->altISA = ( ( c & ( 1 << 6 ) ) != 0 );
                        _stateChange( i, EV_CH_ALTISA );
                    }

                    if ( c & 0x80 )
                    {
                        i->byteCount++;
                    }
                    else
                    {
                        if ( report )
                        {
                            report( V_ERROR, "Exception jump (%d) to 0x%08x" EOL, cpu->exception, cpu->addr );
                        }

                        newState = TRACE_IDLE;
                        retVal = TRACE_EV_MSG_RXED;
                    }
                }
                else
                {
                    if ( c & 0x80 )
                    {
                        /* This is exception byte 1 */
                        cpu->exception |= ( c & 0x1f ) << 4;

                        if ( cpu->hyp != ( ( c & ( 1 << 5 ) ) != 0 ) )
                        {
                            cpu->hyp = ( ( c & ( 1 << 5 ) ) != 0 );
                            _stateChange( i, EV_CH_HYP );
                        }

                        if ( !( c & 0x40 ) )
                        {
                            /* There will not be another one along, return idle */
                            if ( report )
                            {
                                report( V_ERROR, "Exception jump (%d) to 0x%08x" EOL, cpu->exception, cpu->addr );
                            }

                            newState = TRACE_IDLE;
                            retVal = TRACE_EV_MSG_RXED;
                        }
                    }
                    else
                    {
                        /* This is exception byte 2 */
                        cpu->resume = ( c & 0xf );

                        if ( cpu->resume )
                        {
                            _stateChange( i, EV_CH_RESUME );
                        }

                        /* Exception byte 2 is always the last one, return */

                        if ( report )
                        {
                            report( V_ERROR, "Exception jump %s(%d) to 0x%08x" EOL, cpu->resume ? "with resume " : "", cpu->exception, cpu->addr );
                        }

                        newState = TRACE_IDLE;
                        retVal = TRACE_EV_MSG_RXED;
                    }
                }

                break;


            // -----------------------------------------------------
            // VMID RELATED ACTIVITIES
            // -----------------------------------------------------
            case TRACE_GET_VMID: /* Collecting virtual machine ID */
                if ( cpu->vmid != c )
                {
                    cpu->vmid = c;
                    _stateChange( i, EV_CH_VMID );
                }

                if ( report )
                {
                    report( V_ERROR, "VMID Set to (%d)" EOL, cpu->vmid );
                }

                newState = TRACE_IDLE;
                retVal = TRACE_EV_MSG_RXED;
                break;

            // -----------------------------------------------------
            // TIMESTAMP RELATED ACTIVITIES
            // -----------------------------------------------------

            case TRACE_GET_TSTAMP: /* Collecting current timestamp */
                if ( i->byteCount < 8 )
                {
                    i->tsConstruct = ( i->tsConstruct & ( ~( 0x7F << i->byteCount ) ) ) | ( ( c & 0x7f ) << i->byteCount );
                }
                else
                {
                    i->tsConstruct = ( i->tsConstruct & ( ~( 0xff << i->byteCount ) ) ) | ( ( c & 0xff ) << i->byteCount );
                }

                i->byteCount++;

                if ( ( !( c & 0x80 ) ) || ( i->byteCount == 9 ) )
                {
                    newState = TRACE_IDLE;
                    cpu->ts = i->tsConstruct;
                    _stateChange( i, EV_CH_TSTAMP );

                    if ( report )
                    {
                        report( V_ERROR, "CPU Timestamp %d" EOL, cpu->ts );
                    }

                    retVal = TRACE_EV_MSG_RXED;
                }

                break;

            // -----------------------------------------------------
            // CYCLECOUNT RELATED ACTIVITIES
            // -----------------------------------------------------

            case TRACE_GET_CYCLECOUNT: /* Collecting cycle count as standalone packet */
                i->cycleConstruct = ( i->cycleConstruct & ~( 0x7f << ( ( i->byteCount ) * 7 ) ) ) | ( ( c & 0x7f ) << ( ( i->byteCount ) * 7 ) );
                i->byteCount++;

                if ( ( !( c & ( 1 << 7 ) ) ) || ( i->byteCount == 5 ) )
                {
                    newState = TRACE_IDLE;
                    cpu->cycleCount = i->cycleConstruct;
                    _stateChange( i, EV_CH_CYCLECOUNT );

                    if ( report )
                    {
                        report( V_ERROR, "Cyclecount %d" EOL, cpu->cycleCount );
                    }

                    retVal = TRACE_EV_MSG_RXED;
                }

                break;


            // -----------------------------------------------------
            // CONTEXTID RELATED ACTIVITIES
            // -----------------------------------------------------

            case TRACE_GET_CONTEXTID: /* Collecting contextID */
                i->contextConstruct = i->contextConstruct + ( c << ( 8 * i->byteCount ) );
                i->byteCount++;

                if ( i->byteCount == i->contextBytes )
                {
                    if ( cpu->contextID != i->contextConstruct )
                    {
                        cpu->contextID = i->contextConstruct;
                        _stateChange( i, EV_CH_CONTEXTID );
                    }

                    if ( report )
                    {
                        report( V_ERROR, "CPU ContextID %d" EOL, cpu->contextID );
                    }

                    retVal = TRACE_EV_MSG_RXED;
                    newState = TRACE_IDLE;
                }

                break;


            // -----------------------------------------------------
            // I-SYNC RELATED ACTIVITIES
            // -----------------------------------------------------

            case TRACE_WAIT_ISYNC:
                if ( c == 0b00001000 )
                {
                    if ( !i->rxedISYNC )
                    {
                        retVal = TRACE_EV_SYNCED;
                        i->rxedISYNC = true;
                    }

                    i->byteCount = i->contextBytes;
                    i->contextConstruct = 0;
                    newState = i->contextBytes ? TRACE_GET_CONTEXTBYTE : TRACE_GET_INFOBYTE;
                }

                break;

            // -----------------------------------------------------

            case TRACE_GET_CONTEXTBYTE: /* Collecting I-Sync contextID bytes */
                i->contextConstruct = i->contextConstruct + ( c << ( 8 * i->byteCount ) );
                i->byteCount++;

                if ( i->byteCount == i->contextBytes )
                {
                    if ( cpu->contextID != i->contextConstruct )
                    {
                        cpu->contextID = i->contextConstruct;
                        _stateChange( i, EV_CH_CONTEXTID );
                    }

                    newState = TRACE_GET_INFOBYTE;
                }

                break;

            // -----------------------------------------------------

            case TRACE_GET_INFOBYTE: /* Collecting I-Sync Information byte */
                if ( ( ( c & 0x10000000 ) != 0 ) != cpu->isLSiP )
                {
                    cpu->isLSiP = ( c & 0x10000000 ) != 0;
                    _stateChange( i, EV_CH_ISLSIP );
                }

                if ( cpu->reason != ( ( c & 0x01100000 ) >> 5 ) )
                {
                    cpu->reason    = ( c & 0x01100000 ) >> 5;
                    _stateChange( i, EV_CH_REASON );
                }

                if ( cpu->jazelle   != ( ( c & 0x00010000 ) != 0 ) )
                {
                    cpu->jazelle   = ( c & 0x00010000 ) != 0;
                    _stateChange( i, EV_CH_JAZELLE );
                }

                if ( cpu->nonSecure != ( ( c & 0x00001000 ) != 0 ) )
                {
                    cpu->nonSecure = ( c & 0x00001000 ) != 0;
                    _stateChange( i, EV_CH_SECURE );
                }

                if ( cpu->altISA != ( ( c & 0x00000100 ) != 0 ) )
                {
                    cpu->altISA    = ( c & 0x00000100 ) != 0;
                    _stateChange( i, EV_CH_ALTISA );
                }

                if ( cpu->hyp != ( ( c & 0x00000010 ) != 0 ) )
                {
                    cpu->hyp       = ( c & 0x00000010 ) != 0;
                    _stateChange( i, EV_CH_HYP );
                }

                i->byteCount = 0;

                if ( i->dataOnlyMode )
                {
                    if ( report )
                    {
                        report( V_ERROR, "ISYNC in dataOnlyMode" EOL );
                    }

                    retVal = TRACE_EV_MSG_RXED;
                    newState = TRACE_IDLE;
                }
                else
                {
                    newState = TRACE_GET_IADDRESS;
                }

                break;

            // -----------------------------------------------------

            case TRACE_GET_IADDRESS: /* Collecting I-Sync Address bytes */
                i->addrConstruct = ( i->addrConstruct & ( ~( 0xff << ( 8 * i->byteCount ) ) ) )  | ( c << ( 8 * i->byteCount ) ) ;
                i->byteCount++;

                if ( i->byteCount == 4 )
                {
                    _stateChange( i, EV_CH_ADDRESS );

                    if ( cpu->jazelle )
                    {
                        /* This is Jazelle mode..can ignore the AltISA bit */
                        /* and bit 0 is bit 0 of the address */
                        cpu->addrMode = TRACE_ADDRMODE_JAZELLE;
                        cpu->addr = i->addrConstruct;
                    }
                    else
                    {
                        if ( ( i->addrConstruct & ( 1 << 0 ) ) ^ ( !cpu->thumb ) )
                        {
                            cpu->thumb     = ( c & 0x00000001 ) != 0;
                            _stateChange( i, EV_CH_THUMB );
                        }

                        if ( i->addrConstruct & ( 1 << 0 ) )
                        {
                            cpu->addrMode = TRACE_ADDRMODE_THUMB;
                            i->addrConstruct &= ~( 1 << 0 );
                            cpu->addr = i->addrConstruct;
                        }
                        else
                        {
                            cpu->addrMode = TRACE_ADDRMODE_ARM;
                            cpu->addr = i->addrConstruct & 0xFFFFFFFC;
                        }
                    }

                    if ( cpu->isLSiP )
                    {
                        /* If this is an LSiP packet we need to go get the address */
                        newState = ( i->usingAltAddrEncode ) ? TRACE_COLLECT_BA_ALT_FORMAT : TRACE_COLLECT_BA_STD_FORMAT;
                    }
                    else
                    {
                        if ( report )
                        {
                            report( V_ERROR, "ISYNC with IADDRESS 0x%08x" EOL, cpu->addr );
                        }

                        newState = TRACE_IDLE;
                        retVal = TRACE_EV_MSG_RXED;
                    }
                }

                break;

            // -----------------------------------------------------

            case TRACE_GET_ICYCLECOUNT: /* Collecting cycle count on front of ISYNC packet */
                i->cycleConstruct = ( i->cycleConstruct & ~( 0x7f << ( ( i->byteCount ) * 7 ) ) ) | ( ( c & 0x7f ) << ( ( i->byteCount ) * 7 ) );
                i->byteCount++;

                if ( ( !( c & ( 1 << 7 ) ) ) || ( i->byteCount == 5 ) )
                {
                    /* Now go to get the rest of the ISYNC packet */
                    /* Collect either the context or the Info Byte next */
                    cpu->cycleCount = i->cycleConstruct;
                    i->byteCount = i->contextBytes;
                    i->contextConstruct = 0;
                    _stateChange( i, EV_CH_CYCLECOUNT );
                    newState = i->contextBytes ? TRACE_GET_CONTEXTBYTE : TRACE_GET_INFOBYTE;
                    break;
                }

                break;

                // -----------------------------------------------------

        }
    }

    if ( i->p != TRACE_UNSYNCED )
    {
        if ( report ) report( V_DEBUG, "%02x:%s --> %s %s(%d)", c, ( i->p == TRACE_IDLE ) ? protoStateName[i->p] : "", protoStateName[newState],
                                  ( ( newState == TRACE_IDLE ) ? ( ( retVal == TRACE_EV_NONE ) ? "!!!" : "OK" ) : " : " ), retVal );
    }

    i->p = newState;

    if ( ( retVal != TRACE_EV_NONE ) && ( i->rxedISYNC ) )
    {
        cb( d );
    }
}
// ====================================================================================================
static void _MTBDecoderPumpAction( struct TRACEDecoder *i, uint32_t source, uint32_t dest, traceDecodeCB cb, genericsReportCB report, void *d )

/* Pump next words through the protocol decoder */

{
    enum TRACEprotoState newState = i->p;
    struct TRACECPUState *cpu = &i->cpu;
    enum TRACEDecoderPumpEvent retVal = TRACE_EV_NONE;

    if ( report )
    {
        report( V_ERROR, "[From 0x%08x to 0x%08x]" EOL, source, dest );
    }

    switch ( i->p )
    {
        // -----------------------------------------------------

        case TRACE_UNSYNCED:
            /* For the first instruction we only have the destination */
            /* but we code the exception indication into here so we know we arrived via an exception */
            cpu->nextAddr = ( dest & 0xFFFFFFFE ) | ( source & 1 );

            /* If the low bit of dest was set then this is a start of trace event */
            if ( dest & 1 )
            {
                _stateChange( i, EV_CH_TRACESTART );
            }

            newState = TRACE_IDLE;
            break;

        // -----------------------------------------------------

        case TRACE_IDLE:
            if ( cpu->nextAddr & 1 )
            {
                /* If low bit of nextAddr is set then we got here via an exception */
                _stateChange( i, EV_CH_EX_ENTRY );
            }

            /* If low bit of dest is set then this is a start of trace */
            if ( dest & 1 )
            {
                _stateChange( i, EV_CH_TRACESTART );
            }

            cpu->addr = cpu->nextAddr & 0xFFFFFFFE;
            cpu->nextAddr = ( dest & 0xFFFFFFFE ) | ( source & 1 );
            cpu->toAddr = source & 0xFFFFFFFE;
            cpu->exception = 0; /* We don't known exception cause on a M0 */
            _stateChange( i, EV_CH_ADDRESS );
            _stateChange( i, EV_CH_LINEAR );
            retVal = TRACE_EV_MSG_RXED;
            break;

        // -----------------------------------------------------

        default:
            assert( false );
            break;

            // -----------------------------------------------------

    }

    if ( retVal != TRACE_EV_NONE )
    {
        cb( d );
    }

    i->p = newState;
}
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Externally available routines
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void TRACEDecoderInit( struct TRACEDecoder *i, enum TRACEprotocol protocol, bool usingAltAddrEncodeSet )

/* Reset a TRACEDecoder instance */

{
    memset( i, 0, sizeof( struct TRACEDecoder ) );
    TRACEDecoderZeroStats( i );
    TRACEDecodeUsingAltAddrEncode( i, usingAltAddrEncodeSet );
    TRACEDecodeProtocol( i, protocol );
}
// ====================================================================================================
void TRACEDecodeProtocol( struct TRACEDecoder *i, enum TRACEprotocol protocol )

{
    assert( protocol < TRACE_PROT_LIST_END );
    assert( i );
    i->protocol = protocol;
}
// ====================================================================================================
void TRACEDecodeUsingAltAddrEncode( struct TRACEDecoder *i, bool usingAltAddrEncodeSet )

{
    assert( i );
    i->usingAltAddrEncode = usingAltAddrEncodeSet;
}
// ====================================================================================================

void TRACEDecoderZeroStats( struct TRACEDecoder *i )

{
    assert( i );
    memset( &i->stats, 0, sizeof( struct TRACEDecoderStats ) );
}
// ====================================================================================================
bool TRACEDecoderIsSynced( struct TRACEDecoder *i )

{
    assert( i );
    return i->p != TRACE_UNSYNCED;
}
// ====================================================================================================
struct TRACEDecoderStats *TRACEDecoderGetStats( struct TRACEDecoder *i )
{
    assert( i );
    return &i->stats;
}
// ====================================================================================================
struct TRACECPUState *TRACECPUState( struct TRACEDecoder *i )
{
    return &i->cpu;
}
// ====================================================================================================
bool TRACEStateChanged( struct TRACEDecoder *i, enum TRACEchanges c )
{
    bool r = ( i->cpu.changeRecord & ( 1 << c ) ) != 0;
    i->cpu.changeRecord &= ~( 1 << c );
    return r;
}
// ====================================================================================================
void TRACEDecoderForceSync( struct TRACEDecoder *i, bool isSynced )

/* Force the decoder into a specific sync state */

{
    assert( i );

    if ( i->p == TRACE_UNSYNCED )
    {
        if ( isSynced )
        {
            i->p = TRACE_IDLE;
            i->stats.syncCount++;
        }
    }
    else
    {
        if ( !isSynced )
        {
            i->stats.lostSyncCount++;
            i->asyncCount = 0;
            i->rxedISYNC = false;
            i->p = TRACE_UNSYNCED;
        }
    }
}
// ====================================================================================================
void TRACEDecoderPump( struct TRACEDecoder *i, uint8_t *buf, int len, traceDecodeCB cb, genericsReportCB report, void *d )

{
    assert( i );
    assert( buf );
    assert( cb );

    /* len can arrive as 0 for the case of an unwrapped buffer */

    switch ( i->protocol )
    {
        case TRACE_PROT_ETM35:
            while ( len-- )
            {
                /* ETM processes one octet at a time */
                _ETM35DecoderPumpAction( i, *( buf++ ), cb, report, d );
            }

            break;

        case TRACE_PROT_MTB:
            while ( len > 7 )
            {
                /* MTB processes two words at a time...a from and to address */
                /* (yes, that could be +1 on a uint32_t increment, but I prefer being explicit) */
                _MTBDecoderPumpAction( i, *( uint32_t * )buf, *( uint32_t * )( buf + 4 ), cb, report, d );
                buf += 8;
                len -= 8;
            }

            break;

        default:
            assert( false );
            break;
    }
}
// ====================================================================================================
