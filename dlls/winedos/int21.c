/*
 * DOS interrupt 21h handler
 *
 * Copyright 1993, 1994 Erik Bos
 * Copyright 1996 Alexandre Julliard
 * Copyright 1997 Andreas Mohr
 * Copyright 1998 Uwe Bonnes
 * Copyright 1998, 1999 Ove Kaaven
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/winbase16.h"
#include "dosexe.h"
#include "miscemu.h"
#include "msdos.h"
#include "file.h"
#include "winerror.h"
#include "winuser.h"
#include "wine/unicode.h"
#include "wine/debug.h"

/*
 * FIXME: Delete this reference when all int21 code has been moved to winedos.
 */
extern void WINAPI INT_Int21Handler( CONTEXT86 *context );

WINE_DEFAULT_DEBUG_CHANNEL(int21);


#include "pshpack1.h"

/*
 * Structure for DOS data that can be accessed directly from applications.
 * Real and protected mode pointers will be returned to this structure so
 * the structure must be correctly packed.
 */
typedef struct _INT21_HEAP {
    WORD uppercase_size;             /* Size of the following table in bytes */
    BYTE uppercase_table[128];       /* Uppercase equivalents of chars from 0x80 to 0xff. */

    WORD lowercase_size;             /* Size of the following table in bytes */
    BYTE lowercase_table[256];       /* Lowercase equivalents of chars from 0x00 to 0xff. */

    WORD collating_size;             /* Size of the following table in bytes */
    BYTE collating_table[256];       /* Values used to sort characters from 0x00 to 0xff. */

    WORD filename_size;              /* Size of the following filename data in bytes */
    BYTE filename_reserved1;         /* 0x01 for MS-DOS 3.30-6.00 */
    BYTE filename_lowest;            /* Lowest permissible character value for filename */
    BYTE filename_highest;           /* Highest permissible character value for filename */
    BYTE filename_reserved2;         /* 0x00 for MS-DOS 3.30-6.00 */
    BYTE filename_exclude_first;     /* First illegal character in permissible range */
    BYTE filename_exclude_last;      /* Last illegal character in permissible range */
    BYTE filename_reserved3;         /* 0x02 for MS-DOS 3.30-6.00 */
    BYTE filename_illegal_size;      /* Number of terminators in the following table */
    BYTE filename_illegal_table[16]; /* Characters which terminate a filename */

    WORD dbcs_size;                  /* Number of valid ranges in the following table */
    BYTE dbcs_table[16];             /* Start/end bytes for N ranges and 00/00 as terminator */

    BYTE misc_indos;                 /* Interrupt 21 nesting flag */
} INT21_HEAP;

#include "poppack.h"


/***********************************************************************
 *           INT21_GetSystemCountryCode
 *
 * Return DOS country code for default system locale.
 */
static WORD INT21_GetSystemCountryCode()
{
    /*
     * FIXME: Determine country code. We should probably use
     *        DOSCONF structure for that.
     */
    return GetSystemDefaultLangID();
}


/***********************************************************************
 *           INT21_FillCountryInformation
 *
 * Fill 34-byte buffer with country information data using
 * default system locale.
 */
static void INT21_FillCountryInformation( BYTE *buffer )
{
    /* 00 - WORD: date format
     *          00 = mm/dd/yy
     *          01 = dd/mm/yy
     *          02 = yy/mm/dd
     */
    *(WORD*)(buffer + 0) = 0; /* FIXME: Get from locale */

    /* 02 - BYTE[5]: ASCIIZ currency symbol string */
    buffer[2] = '$'; /* FIXME: Get from locale */
    buffer[3] = 0;

    /* 07 - BYTE[2]: ASCIIZ thousands separator */
    buffer[7] = 0; /* FIXME: Get from locale */
    buffer[8] = 0;

    /* 09 - BYTE[2]: ASCIIZ decimal separator */
    buffer[9]  = '.'; /* FIXME: Get from locale */
    buffer[10] = 0;

    /* 11 - BYTE[2]: ASCIIZ date separator */
    buffer[11] = '/'; /* FIXME: Get from locale */
    buffer[12] = 0;

    /* 13 - BYTE[2]: ASCIIZ time separator */
    buffer[13] = ':'; /* FIXME: Get from locale */
    buffer[14] = 0;

    /* 15 - BYTE: Currency format
     *          bit 2 = set if currency symbol replaces decimal point
     *          bit 1 = number of spaces between value and currency symbol
     *          bit 0 = 0 if currency symbol precedes value
     *                  1 if currency symbol follows value
     */
    buffer[15] = 0; /* FIXME: Get from locale */

    /* 16 - BYTE: Number of digits after decimal in currency */
    buffer[16] = 0; /* FIXME: Get from locale */

    /* 17 - BYTE: Time format
     *          bit 0 = 0 if 12-hour clock
     *                  1 if 24-hour clock
     */
    buffer[17] = 1; /* FIXME: Get from locale */

    /* 18 - DWORD: Address of case map routine */
    *(DWORD*)(buffer + 18) = 0; /* FIXME: ptr to case map routine */

    /* 22 - BYTE[2]: ASCIIZ data-list separator */
    buffer[22] = ','; /* FIXME: Get from locale */
    buffer[23] = 0;

    /* 24 - BYTE[10]: Reserved */
    memset( buffer + 24, 0, 10 );
}


/***********************************************************************
 *           INT21_FillHeap
 *
 * Initialize DOS heap.
 */
static void INT21_FillHeap( INT21_HEAP *heap )
{
    static const char terminators[] = "\"\\./[]:|<>+=;,";
    int i;

    /*
     * Uppercase table.
     */
    heap->uppercase_size = 128;
    for (i = 0; i < 128; i++) 
        heap->uppercase_table[i] = toupper( 128 + i );

    /*
     * Lowercase table.
     */
    heap->lowercase_size = 256;
    for (i = 0; i < 256; i++) 
        heap->lowercase_table[i] = tolower( i );
    
    /*
     * Collating table.
     */
    heap->collating_size = 256;
    for (i = 0; i < 256; i++) 
        heap->collating_table[i] = i;

    /*
     * Filename table.
     */
    heap->filename_size = 8 + strlen(terminators);
    heap->filename_illegal_size = strlen(terminators);
    strcpy( heap->filename_illegal_table, terminators );

    heap->filename_reserved1 = 0x01;
    heap->filename_lowest = 0;           /* FIXME: correct value? */
    heap->filename_highest = 0xff;       /* FIXME: correct value? */
    heap->filename_reserved2 = 0x00;    
    heap->filename_exclude_first = 0x00; /* FIXME: correct value? */
    heap->filename_exclude_last = 0x00;  /* FIXME: correct value? */
    heap->filename_reserved3 = 0x02;

    /*
     * DBCS lead byte table. This table is empty.
     */
    heap->dbcs_size = 0;
    memset( heap->dbcs_table, 0, sizeof(heap->dbcs_table) );

    /*
     * Initialize InDos flag.
     */
    heap->misc_indos = 0;
}


/***********************************************************************
 *           INT21_GetHeapSelector
 *
 * Get segment/selector for DOS heap (INT21_HEAP).
 * Creates and initializes heap on first call.
 */
static WORD INT21_GetHeapSelector( CONTEXT86 *context )
{
    static WORD heap_segment = 0;
    static WORD heap_selector = 0;
    static BOOL heap_initialized = FALSE;

    if (!heap_initialized)
    {
        INT21_HEAP *ptr = DOSVM_AllocDataUMB( sizeof(INT21_HEAP), 
                                              &heap_segment,
                                              &heap_selector );
        INT21_FillHeap( ptr );
        heap_initialized = TRUE;
    }

    if (!ISV86(context) && DOSVM_IsWin16())
        return heap_selector;
    else
        return heap_segment;
}


/***********************************************************************
 *           INT21_ExtendedCountryInformation
 *
 * Handler for function 0x65.
 */
static void INT21_ExtendedCountryInformation( CONTEXT86 *context )
{
    BYTE *dataptr = CTX_SEG_OFF_TO_LIN( context, context->SegEs, context->Edi );

    TRACE( "GET EXTENDED COUNTRY INFORMATION, subfunction %02x\n",
           AL_reg(context) );

    /*
     * Check subfunctions that are passed country and code page.
     */
    if (AL_reg(context) >= 0x01 && AL_reg(context) <= 0x07)
    {
        WORD country = DX_reg(context);
        WORD codepage = BX_reg(context);

        if (country != 0xffff && country != INT21_GetSystemCountryCode())
            FIXME( "Requested info on non-default country %04x\n", country );

        if (codepage != 0xffff && codepage != GetOEMCP())
            FIXME( "Requested info on non-default code page %04x\n", codepage );
    }

    switch (AL_reg(context)) {
    case 0x00: /* SET GENERAL INTERNATIONALIZATION INFO */
        INT_BARF( context, 0x21 );
        SET_CFLAG( context );
        break;

    case 0x01: /* GET GENERAL INTERNATIONALIZATION INFO */
        TRACE( "Get general internationalization info\n" );
        dataptr[0] = 0x01; /* Info ID */
        *(WORD*)(dataptr+1) = 38; /* Size of the following info */
        *(WORD*)(dataptr+3) = INT21_GetSystemCountryCode(); /* Country ID */
        *(WORD*)(dataptr+5) = GetOEMCP(); /* Code page */
        INT21_FillCountryInformation( dataptr + 7 );
        SET_CX( context, 41 ); /* Size of returned info */
        break;
        
    case 0x02: /* GET POINTER TO UPPERCASE TABLE */
    case 0x04: /* GET POINTER TO FILENAME UPPERCASE TABLE */
        TRACE( "Get pointer to uppercase table\n" );
        dataptr[0] = AL_reg(context); /* Info ID */
        *(DWORD*)(dataptr+1) = MAKESEGPTR( INT21_GetHeapSelector( context ),
                                           offsetof(INT21_HEAP, uppercase_size) );
        SET_CX( context, 5 ); /* Size of returned info */
        break;

    case 0x03: /* GET POINTER TO LOWERCASE TABLE */
        TRACE( "Get pointer to lowercase table\n" );
        dataptr[0] = 0x03; /* Info ID */
        *(DWORD*)(dataptr+1) = MAKESEGPTR( INT21_GetHeapSelector( context ),
                                           offsetof(INT21_HEAP, lowercase_size) );
        SET_CX( context, 5 ); /* Size of returned info */
        break;

    case 0x05: /* GET POINTER TO FILENAME TERMINATOR TABLE */
        TRACE("Get pointer to filename terminator table\n");
        dataptr[0] = 0x05; /* Info ID */
        *(DWORD*)(dataptr+1) = MAKESEGPTR( INT21_GetHeapSelector( context ),
                                           offsetof(INT21_HEAP, filename_size) );
        SET_CX( context, 5 ); /* Size of returned info */
        break;

    case 0x06: /* GET POINTER TO COLLATING SEQUENCE TABLE */
        TRACE("Get pointer to collating sequence table\n");
        dataptr[0] = 0x06; /* Info ID */
        *(DWORD*)(dataptr+1) = MAKESEGPTR( INT21_GetHeapSelector( context ),
                                           offsetof(INT21_HEAP, collating_size) );
        SET_CX( context, 5 ); /* Size of returned info */
        break;

    case 0x07: /* GET POINTER TO DBCS LEAD BYTE TABLE */
        TRACE("Get pointer to DBCS lead byte table\n");
        dataptr[0] = 0x07; /* Info ID */
        *(DWORD*)(dataptr+1) = MAKESEGPTR( INT21_GetHeapSelector( context ),
                                           offsetof(INT21_HEAP, dbcs_size) );
        SET_CX( context, 5 ); /* Size of returned info */
        break;

    case 0x20: /* CAPITALIZE CHARACTER */
    case 0xa0: /* CAPITALIZE FILENAME CHARACTER */
        TRACE("Convert char to uppercase\n");
        SET_DL( context, toupper(DL_reg(context)) );
        break;

    case 0x21: /* CAPITALIZE STRING */
    case 0xa1: /* CAPITALIZE COUNTED FILENAME STRING */
        TRACE("Convert string to uppercase with length\n");
        {
            char *ptr = (char *)CTX_SEG_OFF_TO_LIN( context,
                                                    context->SegDs,
                                                    context->Edx );
            WORD len = CX_reg(context);
            while (len--) { *ptr = toupper(*ptr); ptr++; }
        }
        break;

    case 0x22: /* CAPITALIZE ASCIIZ STRING */
    case 0xa2: /* CAPITALIZE ASCIIZ FILENAME */
        TRACE("Convert ASCIIZ string to uppercase\n");
        _strupr( (LPSTR)CTX_SEG_OFF_TO_LIN(context, context->SegDs, context->Edx) );
        break;

    case 0x23: /* DETERMINE IF CHARACTER REPRESENTS YES/NO RESPONSE */
        INT_BARF( context, 0x21 );
        SET_CFLAG( context );
        break;

    default:
        INT_BARF( context, 0x21 );
        SET_CFLAG(context);
        break;
    }
}


/***********************************************************************
 *           INT21_GetPSP
 *
 * Handler for functions 0x51 and 0x62.
 */
static void INT21_GetPSP( CONTEXT86 *context )
{
    TRACE( "GET CURRENT PSP ADDRESS (%02x)\n", AH_reg(context) );

    /*
     * FIXME: should we return the original DOS PSP upon
     *        Windows startup ? 
     */
    if (!ISV86(context) && DOSVM_IsWin16())
        SET_BX( context, LOWORD(GetCurrentPDB16()) );
    else
        SET_BX( context, DOSVM_psp );
}


/***********************************************************************
 *           INT21_Ioctl
 *
 * Handler for function 0x44.
 */
static void INT21_Ioctl( CONTEXT86 *context )
{
  static const WCHAR emmxxxx0W[] = {'E','M','M','X','X','X','X','0',0};
  const DOS_DEVICE *dev = DOSFS_GetDeviceByHandle(
      DosFileHandleToWin32Handle(BX_reg(context)) );

  if (dev && !strcmpiW( dev->name, emmxxxx0W )) {
    EMS_Ioctl_Handler(context);
    return;
  }

  switch (AL_reg(context))
  {
  case 0x0b: /* SET SHARING RETRY COUNT */
      TRACE("IOCTL - SET SHARING RETRY COUNT pause %d retries %d\n",
           CX_reg(context), DX_reg(context));
      if (!CX_reg(context))
      {
         SET_AX( context, 1 );
         SET_CFLAG(context);
         break;
      }
      DOSMEM_LOL()->sharing_retry_delay = CX_reg(context);
      if (!DX_reg(context))
         DOSMEM_LOL()->sharing_retry_count = DX_reg(context);
      RESET_CFLAG(context);
      break;
  default:
      INT_Int21Handler( context );
  }
}


/***********************************************************************
 *           INT21_GetExtendedError
 */
static void INT21_GetExtendedError( CONTEXT86 *context )
{
    BYTE class, action, locus;
    WORD error = GetLastError();

    switch(error)
    {
    case ERROR_SUCCESS:
        class = action = locus = 0;
        break;
    case ERROR_DIR_NOT_EMPTY:
        class  = EC_Exists;
        action = SA_Ignore;
        locus  = EL_Disk;
        break;
    case ERROR_ACCESS_DENIED:
        class  = EC_AccessDenied;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    case ERROR_CANNOT_MAKE:
        class  = EC_AccessDenied;
        action = SA_Abort;
        locus  = EL_Unknown;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        class  = EC_MediaError;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        class  = EC_Exists;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    case ERROR_FILE_NOT_FOUND:
        class  = EC_NotFound;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    case ER_GeneralFailure:
        class  = EC_SystemFailure;
        action = SA_Abort;
        locus  = EL_Unknown;
        break;
    case ERROR_INVALID_DRIVE:
        class  = EC_MediaError;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    case ERROR_INVALID_HANDLE:
        class  = EC_ProgramError;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    case ERROR_LOCK_VIOLATION:
        class  = EC_AccessDenied;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    case ERROR_NO_MORE_FILES:
        class  = EC_MediaError;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    case ER_NoNetwork:
        class  = EC_NotFound;
        action = SA_Abort;
        locus  = EL_Network;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
        class  = EC_OutOfResource;
        action = SA_Abort;
        locus  = EL_Memory;
        break;
    case ERROR_PATH_NOT_FOUND:
        class  = EC_NotFound;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    case ERROR_SEEK:
        class  = EC_NotFound;
        action = SA_Ignore;
        locus  = EL_Disk;
        break;
    case ERROR_SHARING_VIOLATION:
        class  = EC_Temporary;
        action = SA_Retry;
        locus  = EL_Disk;
        break;
    case ERROR_TOO_MANY_OPEN_FILES:
        class  = EC_ProgramError;
        action = SA_Abort;
        locus  = EL_Disk;
        break;
    default:
        FIXME("Unknown error %d\n", error );
        class  = EC_SystemFailure;
        action = SA_Abort;
        locus  = EL_Unknown;
        break;
    }
    TRACE("GET EXTENDED ERROR code 0x%02x class 0x%02x action 0x%02x locus %02x\n",
           error, class, action, locus );
    SET_AX( context, error );
    SET_BH( context, class );
    SET_BL( context, action );
    SET_CH( context, locus );
}


/***********************************************************************
 *           DOSVM_Int21Handler (WINEDOS16.133)
 *
 * Interrupt 0x21 handler.
 */
void WINAPI DOSVM_Int21Handler( CONTEXT86 *context )
{
    BOOL bSetDOSExtendedError = FALSE;

    TRACE( "AX=%04x BX=%04x CX=%04x DX=%04x "
           "SI=%04x DI=%04x DS=%04x ES=%04x EFL=%08lx\n",
           AX_reg(context), BX_reg(context), 
           CX_reg(context), DX_reg(context),
           SI_reg(context), DI_reg(context),
           (WORD)context->SegDs, (WORD)context->SegEs,
           context->EFlags );

   /*
    * Extended error is used by (at least) functions 0x2f to 0x62.
    * Function 0x59 returns extended error and error should not
    * be cleared before handling the function.
    */
    if (AH_reg(context) >= 0x2f && AH_reg(context) != 0x59) 
        SetLastError(0);

    RESET_CFLAG(context); /* Not sure if this is a good idea. */

    switch(AH_reg(context))
    {
    case 0x00: /* TERMINATE PROGRAM */
        TRACE("TERMINATE PROGRAM\n");
        if (DOSVM_IsWin16())
            ExitThread( 0 );
        else
            MZ_Exit( context, FALSE, 0 );
        break;

    case 0x01: /* READ CHARACTER FROM STANDARD INPUT, WITH ECHO */
        {
            BYTE ascii;
            TRACE("DIRECT CHARACTER INPUT WITH ECHO\n");
            DOSVM_Int16ReadChar(&ascii, NULL, FALSE);
            SET_AL( context, ascii );
            DOSVM_PutChar(AL_reg(context));
        }
        break;

    case 0x02: /* WRITE CHARACTER TO STANDARD OUTPUT */
        TRACE("Write Character to Standard Output\n");
        DOSVM_PutChar(DL_reg(context));
        break;

    case 0x03: /* READ CHARACTER FROM STDAUX  */
    case 0x04: /* WRITE CHARACTER TO STDAUX */
    case 0x05: /* WRITE CHARACTER TO PRINTER */
        INT_BARF( context, 0x21 );
        break;

    case 0x06: /* DIRECT CONSOLE IN/OUTPUT */
        /* FIXME: Use DOSDEV_Peek/Read/Write(DOSDEV_Console(),...) !! */
        if (DL_reg(context) == 0xff) {
            static char scan = 0;
            TRACE("Direct Console Input\n");
            if (scan) {
                /* return pending scancode */
                SET_AL( context, scan );
                RESET_ZFLAG(context);
                scan = 0;
            } else {
                BYTE ascii;
                if (DOSVM_Int16ReadChar(&ascii,&scan,TRUE)) {
                    DOSVM_Int16ReadChar(&ascii,&scan,FALSE);
                    /* return ASCII code */
                    SET_AL( context, ascii );
                    RESET_ZFLAG(context);
                    /* return scan code on next call only if ascii==0 */
                    if (ascii) scan = 0;
                } else {
                    /* nothing pending, clear everything */
                    SET_AL( context, 0 );
                    SET_ZFLAG(context);
                    scan = 0; /* just in case */
                }
            }
        } else {
            TRACE("Direct Console Output\n");
            DOSVM_PutChar(DL_reg(context));
        }
        break;

    case 0x07: /* DIRECT CHARACTER INPUT WITHOUT ECHO */
        {
            BYTE ascii;
            /* FIXME: Use DOSDEV_Peek/Read(DOSDEV_Console(),...) !! */
            TRACE("DIRECT CHARACTER INPUT WITHOUT ECHO\n");
            DOSVM_Int16ReadChar(&ascii, NULL, FALSE);
            SET_AL( context, ascii );
        }
        break;

    case 0x08: /* CHARACTER INPUT WITHOUT ECHO */
        {
            BYTE ascii;
            /* FIXME: Use DOSDEV_Peek/Read(DOSDEV_Console(),...) !! */
            TRACE("CHARACTER INPUT WITHOUT ECHO\n");
            DOSVM_Int16ReadChar(&ascii, NULL, FALSE);
            SET_AL( context, ascii );
        }
        break;

    case 0x09: /* WRITE STRING TO STANDARD OUTPUT */
        INT_Int21Handler( context );
        break;

    case 0x0a: /* BUFFERED INPUT */
        INT_Int21Handler( context );
        break;

    case 0x0b: /* GET STDIN STATUS */
        TRACE( "GET STDIN STATUS\n" );
        {
            BIOSDATA *data = BIOS_DATA;
            if(data->FirstKbdCharPtr == data->NextKbdCharPtr)
                SET_AL( context, 0 );
            else
                SET_AL( context, 0xff );
        }
        break;

    case 0x0c: /* FLUSH BUFFER AND READ STANDARD INPUT */
        {
            BYTE al = AL_reg(context); /* Input function to execute after flush. */

            TRACE( "FLUSH BUFFER AND READ STANDARD INPUT - 0x%02x\n", al );

            /* FIXME: buffers are not flushed */

            /*
             * If AL is one of 0x01, 0x06, 0x07, 0x08, or 0x0a,
             * int21 function identified by AL will be called.
             */
            if(al == 0x01 || al == 0x06 || al == 0x07 || al == 0x08 || al == 0x0a)
            {
                SET_AH( context, al );
                DOSVM_Int21Handler( context );
            }
        }
        break;

    case 0x0d: /* DISK BUFFER FLUSH */
        TRACE("DISK BUFFER FLUSH ignored\n");
        break;

    case 0x0e: /* SELECT DEFAULT DRIVE */
        INT_Int21Handler( context );
        break;

    case 0x0f: /* OPEN FILE USING FCB */
    case 0x10: /* CLOSE FILE USING FCB */
        INT_BARF( context, 0x21 );
        break;

    case 0x11: /* FIND FIRST MATCHING FILE USING FCB */
    case 0x12: /* FIND NEXT MATCHING FILE USING FCB */
        INT_Int21Handler( context );
        break;

    case 0x13: /* DELETE FILE USING FCB */
    case 0x14: /* SEQUENTIAL READ FROM FCB FILE */
    case 0x15: /* SEQUENTIAL WRITE TO FCB FILE */
    case 0x16: /* CREATE OR TRUNCATE FILE USING FCB */
    case 0x17: /* RENAME FILE USING FCB */
        INT_BARF( context, 0x21 );
        break;

    case 0x18: /* NULL FUNCTION FOR CP/M COMPATIBILITY */
        SET_AL( context, 0 );
        break;

    case 0x19: /* GET CURRENT DEFAULT DRIVE */
    case 0x1a: /* SET DISK TRANSFER AREA ADDRESS */
    case 0x1b: /* GET ALLOCATION INFORMATION FOR DEFAULT DRIVE */        
    case 0x1c: /* GET ALLOCATION INFORMATION FOR SPECIFIC DRIVE */
        INT_Int21Handler( context );
        break;

    case 0x1d: /* NULL FUNCTION FOR CP/M COMPATIBILITY */
    case 0x1e: /* NULL FUNCTION FOR CP/M COMPATIBILITY */
        SET_AL( context, 0 );
        break;

    case 0x1f: /* GET DRIVE PARAMETER BLOCK FOR DEFAULT DRIVE */
        INT_Int21Handler( context );
        break;

    case 0x20: /* NULL FUNCTION FOR CP/M COMPATIBILITY */
        SET_AL( context, 0 );
        break;

    case 0x21: /* READ RANDOM RECORD FROM FCB FILE */
    case 0x22: /* WRITE RANDOM RECORD TO FCB FILE */
    case 0x23: /* GET FILE SIZE FOR FCB */
    case 0x24: /* SET RANDOM RECORD NUMBER FOR FCB */
        INT_BARF( context, 0x21 );
        break;

    case 0x25: /* SET INTERRUPT VECTOR */
        TRACE("SET INTERRUPT VECTOR 0x%02x\n",AL_reg(context));
        {
            FARPROC16 ptr = (FARPROC16)MAKESEGPTR( context->SegDs, DX_reg(context) );
            if (!ISV86(context) && DOSVM_IsWin16())
                DOSVM_SetPMHandler16(  AL_reg(context), ptr );
            else
                DOSVM_SetRMHandler( AL_reg(context), ptr );
        }
        break;

    case 0x26: /* CREATE NEW PROGRAM SEGMENT PREFIX */
    case 0x27: /* RANDOM BLOCK READ FROM FCB FILE */
    case 0x28: /* RANDOM BLOCK WRITE TO FCB FILE */
        INT_BARF( context, 0x21 );
        break;

    case 0x29: /* PARSE FILENAME INTO FCB */
        INT_Int21Handler( context );
        break;

    case 0x2a: /* GET SYSTEM DATE */
        TRACE( "GET SYSTEM DATE\n" );
        {
            SYSTEMTIME systime;
            GetLocalTime( &systime );
            SET_CX( context, systime.wYear );
            SET_DH( context, systime.wMonth );
            SET_DL( context, systime.wDay );
            SET_AL( context, systime.wDayOfWeek );
        }
        break;

    case 0x2b: /* SET SYSTEM DATE */
        FIXME("SetSystemDate(%02d/%02d/%04d): not allowed\n",
              DL_reg(context), DH_reg(context), CX_reg(context) );
        SET_AL( context, 0 );  /* Let's pretend we succeeded */
        break;

    case 0x2c: /* GET SYSTEM TIME */
        TRACE( "GET SYSTEM TIME\n" );
        {
            SYSTEMTIME systime;
            GetLocalTime( &systime );
            SET_CH( context, systime.wHour );
            SET_CL( context, systime.wMinute );
            SET_DH( context, systime.wSecond );
            SET_DL( context, systime.wMilliseconds / 10 );
        }
        break;

    case 0x2d: /* SET SYSTEM TIME */
        FIXME("SetSystemTime(%02d:%02d:%02d.%02d): not allowed\n",
              CH_reg(context), CL_reg(context),
              DH_reg(context), DL_reg(context) );
        SET_AL( context, 0 );  /* Let's pretend we succeeded */
        break;

    case 0x2e: /* SET VERIFY FLAG */
        TRACE("SET VERIFY FLAG ignored\n");
        /* we cannot change the behaviour anyway, so just ignore it */
        break;

    case 0x2f: /* GET DISK TRANSFER AREA ADDRESS */ 
        INT_Int21Handler( context );
        break;

    case 0x30: /* GET DOS VERSION */
        TRACE( "GET DOS VERSION - %s requested\n",
               (AL_reg(context) == 0x00) ? "OEM number" : "version flag" );

        SET_AL( context, HIBYTE(HIWORD(GetVersion16())) ); /* major version */
        SET_AH( context, LOBYTE(HIWORD(GetVersion16())) ); /* minor version */

        if (AL_reg(context) == 0x00)
            SET_BH( context, 0xff ); /* OEM number => undefined */
        else
            SET_BH( context, 0x08 ); /* version flag => DOS is in ROM */

        SET_BL( context, 0x12 );     /* 0x123456 is Wine's serial # */
        SET_CX( context, 0x3456 );
        break;

    case 0x31: /* TERMINATE AND STAY RESIDENT */
        FIXME("TERMINATE AND STAY RESIDENT stub\n");
        break;

    case 0x32: /* GET DOS DRIVE PARAMETER BLOCK FOR SPECIFIC DRIVE */
    case 0x33: /* MULTIPLEXED */
        INT_Int21Handler( context );
        break;

    case 0x34: /* GET ADDRESS OF INDOS FLAG */
        TRACE( "GET ADDRESS OF INDOS FLAG\n" );
        context->SegEs = INT21_GetHeapSelector( context );
        SET_BX( context, offsetof(INT21_HEAP, misc_indos) );
        break;

    case 0x35: /* GET INTERRUPT VECTOR */
        TRACE("GET INTERRUPT VECTOR 0x%02x\n",AL_reg(context));
        {
            FARPROC16 addr;
            if (!ISV86(context) && DOSVM_IsWin16())
                addr = DOSVM_GetPMHandler16( AL_reg(context) );
            else
                addr = DOSVM_GetRMHandler( AL_reg(context) );
            context->SegEs = SELECTOROF(addr);
            SET_BX( context, OFFSETOF(addr) );
        }
        break;

    case 0x36: /* GET FREE DISK SPACE */
    case 0x37: /* SWITCHAR */
        INT_Int21Handler( context );
        break;

    case 0x38: /* GET COUNTRY-SPECIFIC INFORMATION */
        TRACE( "GET COUNTRY-SPECIFIC INFORMATION\n" );
        if (AL_reg(context)) 
        {
            WORD country = AL_reg(context);
            if (country == 0xff)
                country = BX_reg(context);
            if (country != INT21_GetSystemCountryCode())
                FIXME( "Requested info on non-default country %04x\n", country );
        }
        INT21_FillCountryInformation( CTX_SEG_OFF_TO_LIN(context, 
                                                         context->SegDs, 
                                                         context->Edx) );
        SET_AX( context, INT21_GetSystemCountryCode() );
        SET_BX( context, INT21_GetSystemCountryCode() );
        break;

    case 0x39: /* "MKDIR" - CREATE SUBDIRECTORY */
    case 0x3a: /* "RMDIR" - REMOVE SUBDIRECTORY */
    case 0x3b: /* "CHDIR" - SET CURRENT DIRECTORY */
    case 0x3c: /* "CREAT" - CREATE OR TRUNCATE FILE */
    case 0x3d: /* "OPEN" - OPEN EXISTING FILE */
    case 0x3e: /* "CLOSE" - CLOSE FILE */
    case 0x3f: /* "READ" - READ FROM FILE OR DEVICE */
        INT_Int21Handler( context );
        break;

    case 0x40:  /* "WRITE" - WRITE TO FILE OR DEVICE */
        TRACE( "WRITE from %04lX:%04X to handle %d for %d byte\n",
               context->SegDs, DX_reg(context),
               BX_reg(context), CX_reg(context) );
        {
            BYTE *ptr = CTX_SEG_OFF_TO_LIN(context, context->SegDs, context->Edx);

            if (!DOSVM_IsWin16() && BX_reg(context) == 1)
            {
                int i;
                for(i=0; i<CX_reg(context); i++)
                    DOSVM_PutChar(ptr[i]);
                SET_AX(context, CX_reg(context));
            }
            else
            {
                HFILE handle = (HFILE)DosFileHandleToWin32Handle(BX_reg(context));
                LONG result = _hwrite( handle, ptr, CX_reg(context) );
                if (result == HFILE_ERROR)
                    bSetDOSExtendedError = TRUE;
                else
                    SET_AX( context, (WORD)result );
            }
        }
        break;

    case 0x41: /* "UNLINK" - DELETE FILE */
    case 0x42: /* "LSEEK" - SET CURRENT FILE POSITION */
    case 0x43: /* FILE ATTRIBUTES */
        INT_Int21Handler( context );
        break;

    case 0x44: /* IOCTL */
        INT21_Ioctl( context );
        break;

    case 0x45: /* "DUP" - DUPLICATE FILE HANDLE */
        TRACE( "DUPLICATE FILE HANDLE %d\n", BX_reg(context) );
        {
            HANDLE handle32;
            HFILE  handle16 = HFILE_ERROR;

            if (DuplicateHandle( GetCurrentProcess(),
                                 DosFileHandleToWin32Handle(BX_reg(context)),
                                 GetCurrentProcess(), 
                                 &handle32,
                                 0, TRUE, DUPLICATE_SAME_ACCESS ))
                handle16 = Win32HandleToDosFileHandle(handle32);

            if (handle16 == HFILE_ERROR)
                bSetDOSExtendedError = TRUE;
            else
                SET_AX( context, handle16 );
        }
        break;

    case 0x46: /* "DUP2", "FORCEDUP" - FORCE DUPLICATE FILE HANDLE */
    case 0x47: /* "CWD" - GET CURRENT DIRECTORY */
        INT_Int21Handler( context );
        break;

    case 0x48: /* ALLOCATE MEMORY */
        TRACE( "ALLOCATE MEMORY for %d paragraphs\n", BX_reg(context) );
        {
            WORD  selector = 0;
            DWORD bytes = (DWORD)BX_reg(context) << 4;

            if (!ISV86(context) && DOSVM_IsWin16())
            {
                DWORD rv = GlobalDOSAlloc16( bytes );
                selector = LOWORD( rv );
            }
            else
                DOSMEM_GetBlock( bytes, &selector );
               
            if (selector)
                SET_AX( context, selector );
            else
            {
                SET_CFLAG(context);
                SET_AX( context, 0x0008 ); /* insufficient memory */
                SET_BX( context, DOSMEM_Available() >> 4 );
            }
        }
	break;

    case 0x49: /* FREE MEMORY */
        TRACE( "FREE MEMORY segment %04lX\n", context->SegEs );
        {
            BOOL ok;
            
            if (!ISV86(context) && DOSVM_IsWin16())
            {
                ok = !GlobalDOSFree16( context->SegEs );

                /* If we don't reset ES_reg, we will fail in the relay code */
                if (ok)
                    context->SegEs = 0;
            }
            else
                ok = DOSMEM_FreeBlock( (void*)((DWORD)context->SegEs << 4) );

            if (!ok)
            {
                TRACE("FREE MEMORY failed\n");
                SET_CFLAG(context);
                SET_AX( context, 0x0009 ); /* memory block address invalid */
            }
	}
        break;

    case 0x4a: /* RESIZE MEMORY BLOCK */
        INT_Int21Handler( context );
        break;

    case 0x4b: /* "EXEC" - LOAD AND/OR EXECUTE PROGRAM */
        {
            BYTE *program = CTX_SEG_OFF_TO_LIN(context, context->SegDs, context->Edx);
            BYTE *paramblk = CTX_SEG_OFF_TO_LIN(context, context->SegEs, context->Ebx);

            TRACE( "EXEC %s\n", program );

            if (DOSVM_IsWin16())
            {
                HINSTANCE16 instance = WinExec16( program, SW_NORMAL );
                if (instance < 32)
                {
                    SET_CFLAG( context );
                    SET_AX( context, instance );
                }
            }
            else
            {
                if (!MZ_Exec( context, program, AL_reg(context), paramblk))
                    bSetDOSExtendedError = TRUE;
            }
        }
        break;

    case 0x4c: /* "EXIT" - TERMINATE WITH RETURN CODE */
        TRACE( "EXIT with return code %d\n", AL_reg(context) );
        if (DOSVM_IsWin16())
            ExitThread( AL_reg(context) );
        else
            MZ_Exit( context, FALSE, AL_reg(context) );
        break;

    case 0x4d: /* GET RETURN CODE */
        TRACE("GET RETURN CODE (ERRORLEVEL)\n");
        SET_AX( context, DOSVM_retval );
        DOSVM_retval = 0;
        break;

    case 0x4e: /* "FINDFIRST" - FIND FIRST MATCHING FILE */
    case 0x4f: /* "FINDNEXT" - FIND NEXT MATCHING FILE */
        INT_Int21Handler( context );
        break;

    case 0x50: /* SET CURRENT PROCESS ID (SET PSP ADDRESS) */
        TRACE("SET CURRENT PROCESS ID (SET PSP ADDRESS)\n");
        DOSVM_psp = BX_reg(context);
        break;

    case 0x51: /* GET PSP ADDRESS */
        INT21_GetPSP( context );
        break;

    case 0x52: /* "SYSVARS" - GET LIST OF LISTS */
        if (!ISV86(context) && DOSVM_IsWin16())
        {
            SEGPTR ptr = DOSMEM_LOL()->wine_pm_lol;
            context->SegEs = SELECTOROF(ptr);
            SET_BX( context, OFFSETOF(ptr) );
        }
        else
        {
            SEGPTR ptr = DOSMEM_LOL()->wine_rm_lol;
            context->SegEs = SELECTOROF(ptr);
            SET_BX( context, OFFSETOF(ptr) );
        }
        break;

    case 0x54: /* Get Verify Flag */
        TRACE("Get Verify Flag - Not Supported\n");
        SET_AL( context, 0x00 );  /* pretend we can tell. 00h = off 01h = on */
        break;

    case 0x56: /* "RENAME" - RENAME FILE */
    case 0x57: /* FILE DATE AND TIME */
        INT_Int21Handler( context );
        break;

    case 0x58: /* GET OR SET MEMORY ALLOCATION STRATEGY */
	TRACE( "GET OR SET MEMORY ALLOCATION STRATEGY, subfunction %d\n", 
               AL_reg(context) );
        switch (AL_reg(context))
        {
        case 0x00: /* GET ALLOCATION STRATEGY */
            SET_AX( context, 1 ); /* low memory best fit */
            break;

        case 0x01: /* SET ALLOCATION STRATEGY */
            TRACE( "Set allocation strategy to %d - ignored\n",
                   BL_reg(context) );
            break;

        default:
            INT_BARF( context, 0x21 );
            break;
        }
        break;

    case 0x59: /* GET EXTENDED ERROR INFO */
        INT21_GetExtendedError( context );
        break;

    case 0x5a: /* CREATE TEMPORARY FILE */
    case 0x5b: /* CREATE NEW FILE */ 
    case 0x5c: /* "FLOCK" - RECORD LOCKING */
    case 0x5d: /* NETWORK 5D */
    case 0x5e: /* NETWORK 5E */
    case 0x5f: /* NETWORK 5F */
    case 0x60: /* "TRUENAME" - CANONICALIZE FILENAME OR PATH */
        INT_Int21Handler( context );
        break;

    case 0x61: /* UNUSED */
        SET_AL( context, 0 );
        break;

    case 0x62: /* GET PSP ADDRESS */
        INT21_GetPSP( context );
        break;

    case 0x63: /* MISC. LANGUAGE SUPPORT */
        switch (AL_reg(context)) {
        case 0x00: /* GET DOUBLE BYTE CHARACTER SET LEAD-BYTE TABLE */
            TRACE( "GET DOUBLE BYTE CHARACTER SET LEAD-BYTE TABLE\n" );
            context->SegDs = INT21_GetHeapSelector( context );
            SET_SI( context, offsetof(INT21_HEAP, dbcs_table) );
            SET_AL( context, 0 ); /* success */
            break;
        }
        break;

    case 0x64: /* OS/2 DOS BOX */
        INT_BARF( context, 0x21 );
        SET_CFLAG(context);
    	break;

    case 0x65: /* EXTENDED COUNTRY INFORMATION */
        INT21_ExtendedCountryInformation( context );
        break;

    case 0x66: /* GLOBAL CODE PAGE TABLE */
        switch (AL_reg(context))
        {
        case 0x01:
            TRACE( "GET GLOBAL CODE PAGE TABLE\n" );
            SET_BX( context, GetOEMCP() );
            SET_DX( context, GetOEMCP() );
            break;
        case 0x02:
            FIXME( "SET GLOBAL CODE PAGE TABLE, active %d, system %d - ignored\n",
                   BX_reg(context), DX_reg(context) );
            break;
        }
        break;

    case 0x67: /* SET HANDLE COUNT */
        TRACE( "SET HANDLE COUNT to %d\n", BX_reg(context) );
        if (SetHandleCount( BX_reg(context) ) < BX_reg(context) )
            bSetDOSExtendedError = TRUE;
        break;

    case 0x68: /* "FFLUSH" - COMMIT FILE */
        TRACE( "FFLUSH - handle %d\n", BX_reg(context) );
        if (!FlushFileBuffers( DosFileHandleToWin32Handle(BX_reg(context)) ))
            bSetDOSExtendedError = TRUE;
        break;

    case 0x69: /* DISK SERIAL NUMBER */
        INT_Int21Handler( context );
        break;

    case 0x6a: /* COMMIT FILE */
        TRACE( "COMMIT FILE - handle %d\n", BX_reg(context) );
        if (!FlushFileBuffers( DosFileHandleToWin32Handle(BX_reg(context)) ))
            bSetDOSExtendedError = TRUE;
        break;

    case 0x6b: /* NULL FUNCTION FOR CP/M COMPATIBILITY */
        SET_AL( context, 0 );
        break;

    case 0x6c: /* EXTENDED OPEN/CREATE */
        INT_Int21Handler( context );
        break;

    case 0x70: /* MSDOS 7 - GET/SET INTERNATIONALIZATION INFORMATION */
        FIXME( "MS-DOS 7 - GET/SET INTERNATIONALIZATION INFORMATION\n" );
        SET_CFLAG( context );
        SET_AL( context, 0 );
        break;

    case 0x71: /* MSDOS 7 - LONG FILENAME FUNCTIONS */
        INT_Int21Handler( context );
        break;

    case 0x73: /* MSDOS7 - FAT32 */
        INT_Int21Handler( context );
        break;

    case 0xdc: /* CONNECTION SERVICES - GET CONNECTION NUMBER */
        TRACE( "CONNECTION SERVICES - GET CONNECTION NUMBER - ignored\n" );
        break;

    case 0xea: /* NOVELL NETWARE - RETURN SHELL VERSION */
        TRACE( "NOVELL NETWARE - RETURN SHELL VERSION - ignored\n" );
        break;

    default:
        INT_BARF( context, 0x21 );
        break;

    } /* END OF SWITCH */

    /* Set general error condition. */
    if (bSetDOSExtendedError)
    {
        SET_AX( context, GetLastError() );
        SET_CFLAG( context );
    }

    /* Print error code if carry flag is set. */
    if (context->EFlags & 0x0001)
        TRACE("failed, error %ld\n", GetLastError() );

    TRACE( "returning: AX=%04x BX=%04x CX=%04x DX=%04x "
           "SI=%04x DI=%04x DS=%04x ES=%04x EFL=%08lx\n",
           AX_reg(context), BX_reg(context), 
           CX_reg(context), DX_reg(context), 
           SI_reg(context), DI_reg(context),
           (WORD)context->SegDs, (WORD)context->SegEs,
           context->EFlags );
}
