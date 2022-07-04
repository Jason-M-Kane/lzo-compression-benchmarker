/*****************************************************************************/
/* File:  test.c - Test of threaded LZO.                                     */
/* Author: Jason Kane                                                        */
/*         Portions (c) Oppenhumer                                           */
/*****************************************************************************/

/* BEAT 4.80 seconds!!! */

//Changing semaphores to volatile had no effect
//Reducing signaling (every X matches send a signal) had no effect. But then again that could be due to a combination of flushing and the block size (256K)
//Doing a litteral signal and a match signal slowed down timing.  Maybe for the same reason above

/* Target CPU Info */
/* L1 Cache is 32kB 4 way set associative */
/* L2 Cache is 6MByte 24 way set associative */


/************/
/* Includes */
/************/
#include <windows.h>
#include <stdio.h>
#include <intrin.h>
#include <xmmintrin.h>
#include <time.h>

LARGE_INTEGER deadTime;
#if 1


/*****************************/
/* Defines & Data Structures */
/*****************************/
#define LZO_HASH_VALUE		0x1824429D
#define CPY_CMD_ARRAY_SIZE	65536			/* 64k Entries */

/* Commands */
#define IDLE				0
#define MATCH_COPY			1
#define END_OF_BLOCK_WRITE	2
#define REINIT				3
#define EXIT				4


#define MY_LZO_MIN(a,b)        ((a) <= (b) ? (a) : (b))

unsigned int G_Matches = 0;

/* Try Param1 Param2 to reduce */
static int command[CPY_CMD_ARRAY_SIZE];
static const char* startOffset[CPY_CMD_ARRAY_SIZE];
static unsigned int litteralLength[CPY_CMD_ARRAY_SIZE];
static unsigned int matchOffset[CPY_CMD_ARRAY_SIZE];
static unsigned int matchLengthArray[CPY_CMD_ARRAY_SIZE];

/* Max Array Address */
#define MAX_CMD_BYTE_ADDR ((unsigned int)&command[0] + (CPY_CMD_ARRAY_SIZE*4)-4)


typedef struct
{
	char* out;					/* Pointer to output buffer. */
	unsigned int* out_len;		/* Length of output buffer, to be filled in by this routine.  */
	HANDLE* pCpySem;
	HANDLE* pCpyRespSem;
}cpyParam;




/***********************/
/* Function Prototypes */
/***********************/
int plzo1x_1_15_compress (const char* in, unsigned int  in_len, 
	                            char* out, unsigned int* out_len, void* wrkmem );
__inline static int do_search
		(	
			const char* in,				/* Pointer to current input block.  Needs to be sequential address from last processed block */
			unsigned int  in_len,		/* Length of current input block in bytes. */
			unsigned int  ti,			/* Number of litterals remaining from the last processed block */
			void* wrkmem				/* Pointer to memory reserved for dictionary usage.  LZO1X employs a 13-bit single depth array for a dictionary. */
		);
static DWORD WINAPI CopyLitteralsAndMatchData(LPVOID lpParam);

unsigned int initThreadedLZO(int action);


LARGE_INTEGER start,end;

static HANDLE cpyRespSem = NULL;
static HANDLE cpySem = NULL;
static cpyParam copyParameters;

/* Note:  For Some reason this seems to run faster on the same cpu.. CACHE??? */

static int* pcommand = &command[0];
static const char** pstartOffset = &startOffset[0];
static unsigned int* plitteralLength = &litteralLength[0];
static unsigned int* pmatchOffset = &matchOffset[0];
static unsigned int* pmatchLength = &matchLengthArray[0];

/***********************************************************************
// public entry point
************************************************************************/
int plzo1x_1_15_compress      ( const char* in, unsigned int  in_len,
                         char* out, unsigned int* out_len,
                         void* wrkmem )
{
    const char* ip = in;
    char* op = out;
    unsigned int l = in_len;
    unsigned int t = 0;			/* Number of uncompressed litterals remaining on input stream */

	copyParameters.out = out;
	copyParameters.out_len = out_len;

	/* Init Copy Thread */
	*pcommand = REINIT;
	ReleaseSemaphore(cpySem,1,NULL);

	/* Check to increment or reset the "Queue" arrays */
	if((unsigned int)(pcommand) < MAX_CMD_BYTE_ADDR)
	{
		pcommand++;
		pstartOffset++;
		plitteralLength++;
		pmatchOffset++;
		pmatchLength++;
	}
	else
	{
		pcommand = &command[0];
		pstartOffset = &startOffset[0];
		plitteralLength = &litteralLength[0];
		pmatchOffset = &matchOffset[0];
		pmatchLength = &matchLengthArray[0];
	}

    while (l > 20)
    {
        unsigned int ll = l;
        unsigned int* ll_end;

		/* Compress Up to 48kB at a time */
		/* This is a limitation of pointers used for the dictionary */
		//#define LZO_MIN(a,b)        ((a) <= (b) ? (a) : (b))
        ll = MY_LZO_MIN(ll, 49152);

		/* Double-Check that > 16 bytes will be compressed */
        ll_end = (unsigned int*)ip + ll;
        if ((ll_end + ((t + ll) >> 5)) <= ll_end || (const char*)(ll_end + ((t + ll) >> 5)) <= ip + ll)
            break;
		
		/* Zero out the memory being used for the dictionary */
		/* 8kB of entries, 16kB total size                   */
        memset(wrkmem, 0, ((unsigned int)1 << /*D_BITS*/13) * sizeof(unsigned short));

		/* Perform Compression, Put remainder in t */
        t = do_search(ip,ll,t,wrkmem);

        ip += ll;			/* Move Input Ptr */
        l  -= ll;			/* Adjust Remaining Length */
    }

	/*******************************************************************************/
	/* Cleanup - Signal Cpy Thread, Wait for Copy Thread to Complete, Free Memory. */
	/*******************************************************************************/
	t += l;

	/* Send End of Block Write Command */
	*pstartOffset = in + in_len - t;
	*plitteralLength = t;
	*pcommand = END_OF_BLOCK_WRITE;
	ReleaseSemaphore(cpySem,1,NULL);

	/* Check to increment or reset the "Queue" arrays */
	if((unsigned int)(pcommand) < MAX_CMD_BYTE_ADDR)
	{
		pcommand++;
		pstartOffset++;
		plitteralLength++;
		pmatchOffset++;
		pmatchLength++;
	}
	else
	{
		pcommand = &command[0];
		pstartOffset = &startOffset[0];
		plitteralLength = &litteralLength[0];
		pmatchOffset = &matchOffset[0];
		pmatchLength = &matchLengthArray[0];
	}

	
//	QueryPerformanceCounter(&start);	
	/* Wait for Output Length to be filled in */
#if 0
	if( WaitForSingleObject(cpyRespSem,INFINITE) != WAIT_OBJECT_0)
	{
		printf("Error, Wait Failed for Response from Copy Thread.\n");
	}
#endif
//	QueryPerformanceCounter(&end);
//	deadTime.QuadPart = end.QuadPart-start.QuadPart+deadTime.QuadPart;

#if 0
	/* Wait for Copy Thread to Terminate */
	exitCode = STILL_ACTIVE;
	while(exitCode != STILL_ACTIVE);
	{
		Sleep(1);
		GetExitCodeThread(cpyHandle, &exitCode);
	}
#endif

#if 0
	CloseHandle(cpyHandle);
#endif
//	CloseHandle(cpySem);
//	CloseHandle(cpyRespSem);
//	_aligned_free(cpyCmdArray);

//printf("RERISU!\n");
//	fprintf(outptr,"\r\n\r\n");
//	fwrite(out,1,*out_len,outptr);
//	fclose(outptr);

	return 0;            //return LZO_E_OK;
}


/* Init Actions */
#define INITIALIZE_LZO	0
#define CLEANUP_LZO		1
unsigned int initThreadedLZO(int action)
{
	static HANDLE cpyHandle = NULL;
	DWORD exitCode;

	if(action == INITIALIZE_LZO)
	{
		/* Elevate Priorities */
		//SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
		//SetThreadAffinityMask(GetCurrentThread(),0x2);

		/* Build the thread Parameters */
		cpySem = CreateSemaphore( 
			NULL,           // default security attributes
			0,              // initial count
			65536,          // maximum count
			NULL			// unnamed semaphore
		);
		if(cpySem == NULL)
		{
			printf("Error in semaphore creation of cpySem.\n");
			return -1;
		}
		cpyRespSem = CreateSemaphore( 
			NULL,           // default security attributes
			0,              // initial count
			65536,          // maximum count
			NULL			// unnamed semaphore
		);
		if(cpyRespSem == NULL)
		{
			printf("Error in semaphore creation of cpyRespSem.\n");
			return -1;
		}

		/* Init the "Queue" arrays */
		pcommand = &command[0];
		pstartOffset = &startOffset[0];
		plitteralLength = &litteralLength[0];
		pmatchOffset = &matchOffset[0];
		pmatchLength = &matchLengthArray[0];

		deadTime.QuadPart = 0;
		copyParameters.pCpySem = &cpySem;
		copyParameters.pCpyRespSem = &cpyRespSem;

		/* Spawn the Copy Thread */
		cpyHandle = CreateThread(NULL,0,CopyLitteralsAndMatchData,(LPVOID)&copyParameters,0,NULL);
		if(cpyHandle == NULL)
		{
			printf("Error in spawning of Copy Thread.\n");
			//_aligned_free(cpyCmdArray);
			return -1;
		}
	}
	else
	{
		/* Send Termination Notice to the memory copy thread */
		*pcommand = EXIT;
		ReleaseSemaphore(cpySem,1,NULL);

		/* Wait for the memory copy thread to complete - Ensures that output length is correct */
		if( WaitForSingleObject(cpyRespSem,INFINITE) != WAIT_OBJECT_0)
		{
			printf("Error, Wait Failed for Response from Copy Thread.\n");
		}

		/* Wait for Copy Thread to Terminate */
		exitCode = STILL_ACTIVE;
		while(exitCode != STILL_ACTIVE);
		{
			Sleep(1);
			GetExitCodeThread(cpyHandle, &exitCode);
		}
		CloseHandle(cpyHandle);
		CloseHandle(cpySem);
		CloseHandle(cpyRespSem);
		cpyHandle = cpySem = cpyRespSem = NULL;

		return ((unsigned int)copyParameters.out_len);
	}

	return 0;
}










/***************************************************************************************************************/
/* Assumption:  This form of LZO favors speed over compression.  Thus most time is spent searching for matches */
/*              If data copies are performed by a seperate process, more time is devoted to searching and the  */
/*              compression process should finish sooner.                                                      */
/***************************************************************************************************************/

__inline static int do_search
		(	
			const char* in,				/* Pointer to current input block.  Needs to be sequential address from last processed block */
			unsigned int  in_len,		/* Length of current input block in bytes. */
			unsigned int  ti,			/* Number of litterals remaining from the last processed block */
			void* wrkmem				/* Pointer to memory reserved for dictionary usage.  LZO1X employs a 13-bit single depth array for a dictionary. */
		)
{
    register const char* currentInputPtr;				//Pointer to a constant Char.  Points to current search location in the input buffer.
    const char* const in_end = in + in_len;				//Data pointed to and pointer are constants
    const char* const ip_end = in + in_len - 20;		//Data pointed to and pointer are constants
    const char* initialIndex;							//Pointer to a constant Char.  Points to the start of the current search.
    unsigned short* const dict = (unsigned short*) wrkmem;	//Constant Ptr to changeable data

	unsigned int numRelease = 0;
    const char* matchPositionAbs;	/* Absolute address of a particular dictionary entry */
    unsigned int matchLength;		/* # of bytes matched */
    unsigned int dataValue;
    unsigned int dictionaryIndex;
	register unsigned int numLitteralsToCopy;
	unsigned int v;
	DWORD bytematch;
	HANDLE *lclCpySem = &cpySem;

	/* Initialize current input pointer and the initial index pointer */
    initialIndex = currentInputPtr = in;

	/* Adjust where the search begins */
	/* If any litterals are remaining in the input stream from the last call to this function, */
	/* maintain a litteral buffer of at least size 5 bytes since the last match.  */
    currentInputPtr += ti < 4 ? 4 - ti : 0;

	while(1)
    {

literal:
		/* Increment the search by at least 1 byte */
		/* If its been a while since a match was found, start jumping around to speed things up. */
        currentInputPtr += 1 + ((currentInputPtr - initialIndex) >> 5);
next:
        if(currentInputPtr >= ip_end)					/* Check for the end of the block input stream */
            break;
        dataValue = *((DWORD*)currentInputPtr);			/* Get new 32-bit data value from the input stream.					*/
        dictionaryIndex = ((dataValue * LZO_HASH_VALUE) >> 19) & 0x1FFF;	/* Determine dictionary index that maps to the new data value.		*/
		matchPositionAbs = in + dict[dictionaryIndex];	/* Obtain absolute address of the current dictionary entry match.	*/
		dict[dictionaryIndex] = currentInputPtr-in;		/* Update dictionary entry to point to the latest value, store relative offset. */
        if(dataValue != *(DWORD*)matchPositionAbs)		/* Check for a match:  Does the last dictionary entry match the new entry?		*/
            goto literal;

		/*****************************************/
		/* If you get here, a match was detected */
		/*****************************************/
        
		/* Update location of initial index, in necessary */
		initialIndex -= ti;		/* Move the pointer back to the start of the leftover litterals from the last call to this fctn */
		ti = 0;					/* Reset remainder of litterals from previous fctn invoation */
		numLitteralsToCopy = currentInputPtr - initialIndex;	/* Determine the number of litterals to copy */

        
		/*************************************************************************/
		/* Determine the match length by comparing successive 32-bit data values */
		/*************************************************************************/
        /* compare speedup? */
		matchLength = 4;
		v = *(DWORD*)(currentInputPtr + matchLength) ^ *(DWORD*)(matchPositionAbs + matchLength);	/* XOR, V == 0 is match */
		if(v == 0)
		{
			do 
			{
				matchLength += 4;
				v = *(DWORD*)(currentInputPtr + matchLength) ^ *(DWORD*)(matchPositionAbs + matchLength);
				if(currentInputPtr + matchLength >= ip_end)
					goto m_len_done;
			}while (v == 0);
		}

		/* Find the extra # of matched bytes:  Count # of sequential 0 bits then divide by 8 */
        /* ASM Version for speedup? */		
		_BitScanForward(&bytematch,v);	/* ASM BSF */
		matchLength += (bytematch/8);

		/****************************************************************/
		/* Now have the Start Offset, Litteral Length, and Match Length */
		/* Pass this information out to the output thread               */
		/****************************************************************/

m_len_done:
		
		/***************************************************/
		/* Prepare next command to send to the copy thread */
		/***************************************************/
#if 0
		while(cpyCmdArray[*cmdIndex].command != 0)
		{
			Sleep(10);
		}
#endif
		/* Assume other thread is keeping up */
		*pcommand = MATCH_COPY;
		*pmatchOffset = currentInputPtr - matchPositionAbs;
		*pmatchLength = matchLength;
		*pstartOffset = initialIndex;
		*plitteralLength = numLitteralsToCopy;
		numRelease++;
//		ReleaseSemaphore(*cpySem,1,NULL);
		if(numRelease >= 10000)
		{
			ReleaseSemaphore(*lclCpySem,numRelease,NULL);
			numRelease = 0;
		}


		/* Check to increment or reset the "Queue" arrays */
		if((unsigned int)(pcommand) < MAX_CMD_BYTE_ADDR)
		{
			pcommand++;
			pstartOffset++;
			plitteralLength++;
			pmatchOffset++;
			pmatchLength++;
		}
		else
		{
			pcommand = &command[0];
			pstartOffset = &startOffset[0];
			plitteralLength = &litteralLength[0];
			pmatchOffset = &matchOffset[0];
			pmatchLength = &matchLengthArray[0];
		}

		/* Update the Current Input Pointer and Initial Index for the next search */
		currentInputPtr += matchLength;
        initialIndex = currentInputPtr;
		goto next;
	}

	/**************************************************************************************************/
	/* End of Current Input Block stream reached, Return the # of litterals left in the input stream  */
	/**************************************************************************************************/
	if(numRelease > 0)
	{
		ReleaseSemaphore(*lclCpySem,numRelease,NULL);
	}

    return (in_end - (initialIndex-ti) );
}




#if 1
#define M2_MAX_LEN		8
#define M2_MAX_OFFSET	0x800
#define M3_MAX_OFFSET	0x4000

#define M3_MARKER	0x20
#define M4_MARKER	0x10
#define M3_MAX_LEN	33
#define M4_MAX_LEN	0x9
#endif
static DWORD WINAPI CopyLitteralsAndMatchData(LPVOID lpParam)
{
	int iteration = 0;
    int* LCL_pcommand = &command[0];
    const char** LCL_pstartOffset = &startOffset[0];
	unsigned int* LCL_plitteralLength = &litteralLength[0];
	unsigned int* LCL_pmatchOffset = &matchOffset[0];
	unsigned int* LCL_pmatchLength = &matchLengthArray[0];

	char* out;
	char* op;	 //Points to current location being worked on in the output buffer.
	unsigned int* out_len;
	cpyParam* copyParameters = (cpyParam*)lpParam;
	HANDLE *cpySem,*cpyRespSem;
#if 1
	if(iteration == 0)
	{
		//SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
		SetThreadAffinityMask(GetCurrentThread(),0x2);
		iteration = 1;
	}
#endif
	cpyRespSem = copyParameters->pCpyRespSem;
	cpySem = copyParameters->pCpySem;

	while(1)
	{
		/* Wait for a Copy Command to arrive to the thread */


		while( cpySem != 1);

#if 0
		__asm
		{
spin_loop:
			pause;
			mov		EAX,1;
			xchg	EAX,G_;
			cmp		EAX,1;		/* */
			jne spin_loop;
		}
#endif

#if 0
		if(WaitForSingleObject(*cpySem,INFINITE) != WAIT_OBJECT_0)
		{
			printf("Error, Wait Failed in Copy Thread.\n");
		}
#endif
		G_Matches++;

		/* Switch on incoming command */
		switch(*LCL_pcommand)
		{
			case REINIT:
			{
				op = out = copyParameters->out;
				out_len = copyParameters->out_len;
			}
			break;

			case MATCH_COPY:
			{
				register unsigned int litteralLength;
				unsigned int m_len,m_off;
				const char* initialIndex;

				/***************************************************************************/
				/* Perform copy of litterals to the output stream.                         */
				/* ----------------------------------------------------------------------- */
				/*                                                                         */
				/* Encoding for 1 to 3 litterals to be copied:                             */
				/* _______________________________________________________________________ */
				/* | Byte -2     |  Copy # of litterals to copy into the lower 2 bits.   | */
				/* |             |  These two bits are unused bits from the last match.  | */
				/* |             |  Its not possible to get here if a match was not      | */
				/* |             |  previously made.                                     | */
				/* ======================================================================= */
				/* | Byte 0 to N | Litterals                                             | */
				/* ======================================================================= */
				/*                                                                         */
				/* 		                                                                   */
				/* Encoding for 4 to 18 litterals to be copied:                            */
				/* _______________________________________________________________________ */
				/* | Byte 0      |  Number of litterals copied in bytes minus 3          | */
				/* ======================================================================= */
				/* | Byte 1 to N | Litterals                                             | */
				/* ======================================================================= */
				/*                                                                         */
				/*                                                                         */
				/* Encoding for 19+ litterals to be copied:                                */
				/* _______________________________________________________________________ */
				/* | Byte 0 to A  |  Write 0x00 on the output for each time 255 is able  | */
				/* |              |  To divide without fraction into (# litterals to be  | */
				/* |              |  copied minus 18)                                    | */
				/* ======================================================================= */
				/* | Byte A+1     | Litterals                                            | */
				/* |      to N    |                                                      | */
				/* ======================================================================= */
				/*                                                                         */
				/***************************************************************************/
				litteralLength = *LCL_plitteralLength;
				initialIndex = *LCL_pstartOffset;
				m_len = *LCL_pmatchLength;
				m_off = *LCL_pmatchOffset;

				if(litteralLength != 0)
				{
					if (litteralLength <= 3)
					{
						op[-2] |= (char)(litteralLength);
						*(DWORD*)op = *(DWORD*)initialIndex;
						op += litteralLength;
					}
					else if (litteralLength <= 16)
					{
						*op++ = (char)(litteralLength - 3);
						*(DWORD*) op = *(DWORD*)initialIndex;
						*(DWORD*)(op+4) = *(DWORD*)(initialIndex+4);
						*(DWORD*)(op+8) = *(DWORD*)(initialIndex+8);
						*(DWORD*)(op+12) = *(DWORD*)(initialIndex+12);
						op += litteralLength;
					}
					else
					{
						if (litteralLength <= 18)
							*op++ = (char)(litteralLength - 3);
						else
						{
							register unsigned int tt = litteralLength - 18;
							*op++ = 0;
							while(tt > 255)
							{
								tt -= 255;
								* (volatile unsigned char *) op++ = 0;
							}
							*op++ = (char)(tt);
						}

						do {
							*(DWORD*)op = *(DWORD*)initialIndex;
							*(DWORD*)(op+4) = *(DWORD*)(initialIndex+4);
							*(DWORD*)(op+8) = *(DWORD*)(initialIndex+8);
							*(DWORD*)(op+12) = *(DWORD*)(initialIndex+12);
							op += 16; initialIndex += 16; litteralLength -= 16;
						} while (litteralLength >= 16); 
						if (litteralLength > 0)
						{ 
							do 
								*op++ = *initialIndex++; 
							while (--litteralLength > 0); 
						}
					}
				}


				/***************************************************************************/
				/* Write Match Information to the output stream.                           */
				/* ----------------------------------------------------------------------- */
				/*                                                                         */
				/* Encoding for:                                                           */
				/*     1.) Match Length >= 4 And <= 8; 4-bit offset required.              */
				/*     2.) Match offset <= 0x800 (2kB); 12-bit offset required.            */
				/*                                                                         */
				/* Encoded Values:                                                         */
				/*     M_Len: The match length is modified to be length-1 (to fit within   */
				/*            3 bits).                                                     */
				/*     M_Off: The match offset is modified to be offset-1 (to fit within   */
				/*            11 bits).                                                    */
				/*     SmaLL: Small litteral non-matches (1 to 3 bytes) from future        */
				/*            iterations are encoded in a portion of the match info.       */
				/*            If the next literal length is > 3, this field is unused.     */
				/*                                                                         */
				/* The length/offset are stored in the following manner:                   */
				/* _______________________________________________________________________ */
				/* | Byte 0      |         [    M_Len    ]  [  M_Off(2:0)  ]   [ SmaLL ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/* | Byte 1      |         [                M_Off(10:3)                ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/*                                                                         */
				/*                                                                         */
				/* Encoding for:                                                           */
				/*     1.) Match Length >= 4 And <= 33; 6-bit offset required.             */
				/*     2.) Match offset <= 0x4000 (16kB); 15-bit offset required.          */
				/*                                                                         */
				/* Encoded Values:                                                         */
				/*     M_Len: The match length is set to {0x20 | [match length - 2]}       */
				/*            Note: 0x20 serves as the M3 "match block type" identifier.   */
				/*     M_Off: The match offset is modified to be offset-1 (to fit within   */
				/*            14 bits).                                                    */
				/*     SmaLL: Small litteral non-matches (1 to 3 bytes) from future        */
				/*            iterations are encoded in a portion of the match info.       */
				/*            If the next literal length is > 3, this field is unused.     */
				/*                                                                         */
				/* The length/offset are stored in the following manner:                   */
				/* _______________________________________________________________________ */
				/* | Byte 0      |         [        M_Len / M3 Mark Combination        ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/* | Byte 1      |         [           M_Off(5:0)          ]   [ SmaLL ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/* | Byte 2      |         [                M_Off(13:6)                ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/*                                                                         */
				/*                                                                         */
				/* Encoding for:                                                           */
				/*     1.) Match Length >= 34.                                             */
				/*     2.) Match offset <= 0x4000 (16kB); 15-bit offset required.          */
				/*                                                                         */
				/* Encoded Values:                                                         */
				/*     M3_Mark: Constant Value 0x2.  Identifies the match block type.      */
				/*     Spare:   Set equal to 0x0.                                          */
				/*     M_Len: The match length is modified to be length-33.  The value     */
				/*            0x00 is written on the output stream for each time 255       */
				/*            divides evenly into the length.  255 is subtracted from the  */
				/*            length on each such iteration.  Finally, the remaining value */
				/*            [match length] is written to the output stream.              */
				/*     M_Off: The match offset is modified to be offset-1 (to fit within   */
				/*            14 bits).                                                    */
				/*     SmaLL: Small litteral non-matches (1 to 3 bytes) from future        */
				/*            iterations are encoded in a portion of the match info.       */
				/*            If the next literal length is > 3, this field is unused.     */
				/*                                                                         */
				/* The length/offset are stored in the following manner:                   */
				/* _______________________________________________________________________ */
				/* | Byte 0      |         [      M3_Mark      ]   [        Spare       ]| */
				/* |             |  Bits:   7     6     5     4     3      2     1     0 | */
				/* ======================================================================= */
				/* | Byte 1 to A |         [                   M_Len                   ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/* | Byte A+1    |         [           M_Off(5:0)          ]   [ SmaLL ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/* | Byte A+2    |         [                M_Off(13:6)                ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/*                                                                         */
				/*                                                                         */
				/* Encoding for:                                                           */
				/*     1.) Match Length >= 4 And <= 9; 4-bit offset required.              */
				/*     2.) Match offset > 0x4000 (16kB). 15-bit offset required.           */
				/*     Note: Caller limits match offset by limiting the input buffer size. */
				/*     Max offset is 48kB - 4 bytes, which after subtracting 16kB, is      */
				/*     representable by 15 bits.                                           */
				/*                                                                         */
				/* Encoded Values:                                                         */
				/*     M4_Mark: Constant Value 0x1.  Identifies the match block type.      */
				/*     M_Len: The match length is set to length-2 (to fit in 3 bits).      */
				/*     M_Off: The match offset is modified to be offset-0x4000             */
				/*            (to fit within 15 bits).                                     */
				/*     SmaLL: Small litteral non-matches (1 to 3 bytes) from future        */
				/*            iterations are encoded in a portion of the match info.       */
				/*            If the next literal length is > 3, this field is unused.     */
				/*                                                                         */
				/* The length/offset are stored in the following manner:                   */
				/* _______________________________________________________________________ */
				/* | Byte 0      |         [      M4_Mark     ] [Moff(14)] [   M_Len   ] | */
				/* |             |  Bits:   7     6     5     4     3      2     1     0 | */
				/* ======================================================================= */
				/* | Byte 1      |         [           M_Off(5:0)          ]   [ SmaLL ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/* | Byte 2      |         [                M_Off(13:6)                ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/*                                                                         */
				/*                                                                         */
				/* Encoding for:                                                           */
				/*     1.) Match Length > 9.                                               */
				/*     2.) Match offset > 0x4000 (16kB). 15-bit offset required.           */
				/*     Note: Caller limits match offset by limiting the input buffer size. */
				/*     Max offset is 48kB - 4 bytes, which after subtracting 16kB, is      */
				/*     representable by 15 bits.                                           */
				/*                                                                         */
				/* Encoded Values:                                                         */
				/*     M4_Mark: Constant Value 0x1.  Identifies the match block type.      */
				/*     Spare:   Set equal to 0x0.                                          */
				/*     M_Len: The match length is modified to be length-9.  The value      */
				/*            0x00 is written on the output stream for each time 255       */
				/*            divides evenly into the length.  255 is subtracted from the  */
				/*            length on each such iteration.  Finally, the remaining value */
				/*            [match length] is written to the output stream.              */
				/*     M_Off: The match offset is modified to be offset-0x4000             */
				/*            (to fit within 15 bits).                                     */
				/*     SmaLL: Small litteral non-matches (1 to 3 bytes) from future        */
				/*            iterations are encoded in a portion of the match info.       */
				/*            If the next literal length is > 3, this field is unused.     */
				/*                                                                         */
				/* The length/offset are stored in the following manner:                   */
				/* _______________________________________________________________________ */
				/* | Byte 0      |         [      M4_Mark     ] [Moff(14)] [   Spare   ] | */
				/* |             |  Bits:   7     6     5     4     3      2     1     0 | */
				/* ======================================================================= */
				/* | Byte 1 to A |         [                   M_Len                   ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/* | Byte A+1    |         [           M_Off(5:0)          ]   [ SmaLL ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/* | Byte A+2    |         [                M_Off(13:6)                ] | */
				/* |             |  Bits:   7     6     5     4     3     2     1     0  | */
				/* ======================================================================= */
				/*                                                                         */
				/***************************************************************************/
				if (m_len <= M2_MAX_LEN && m_off <= M2_MAX_OFFSET)
				{
					m_off -= 1;
					*op++ = (char)(((m_len - 1) << 5) | ((m_off & 7) << 2));
					*op++ = (char)(m_off >> 3);
				}
				else if (m_off <= M3_MAX_OFFSET)
				{
					m_off -= 1;
					if (m_len <= M3_MAX_LEN)
						*op++ = (char)(M3_MARKER | (m_len - 2));
					else
					{
						m_len -= M3_MAX_LEN;
						*op++ = M3_MARKER | 0;
						while(m_len > 255)
						{
							m_len -= 255;
							* (volatile unsigned char *) op++ = 0;
						}
						*op++ = (char)(m_len);
					}
					*op++ = (char)(m_off << 2);
					*op++ = (char)(m_off >> 6);
				}
				else
				{
					m_off -= 0x4000;
					if (m_len <= M4_MAX_LEN)
						*op++ = (char)(M4_MARKER | ((m_off >> 11) & 8) | (m_len - 2));
					else
					{
						m_len -= M4_MAX_LEN;
						*op++ = (char)(M4_MARKER | ((m_off >> 11) & 8));
						while(m_len > 255)
						{
							m_len -= 255;
							*op++ = 0;
						}
						*op++ = (char)(m_len);
					}
					*op++ = (char)(m_off << 2);
					*op++ = (char)(m_off >> 6);
				}
			}
			break;


			case END_OF_BLOCK_WRITE:
			{
				unsigned int litteralLength;
				const char* initialIndex;
				litteralLength = *LCL_plitteralLength;
				initialIndex = *LCL_pstartOffset;

				/*************************************************************************/
				/* Perform a copy of the remaining litterals that have no dict matches.  */
				/*                                                                       */
				/* t is the number of litterals remaining from the last compression      */
				/* attempt plus leftover (l).                                            */
				/* Note that compression is only performed on blocks of data > 20 bytes. */
				/*                                                                       */
				/* Case 1:  No matches were found and t <= 238 bytes                     */
				/*			Write 17+t to the output stream.                             */
				/*          Write "t" litterals to the output stream.                    */
				/*          Write the following sequence of bytes: M4_MARKER|1,0,0       */
				/*                                                                       */
				/* Case 2:  Matches were found And t <= 3 bytes                          */
				/*			Write t to current output ptr[-2].                           */
				/*          Write "t" litterals to the output stream.                    */
				/*          Write the following sequence of bytes: M4_MARKER|1,0,0       */
				/*                                                                       */
				/* Case 3:  Matches were found And 4 <= t <= 18 bytes                    */
				/*			Write t-3 to the output stream.                              */
				/*          Write "t" litterals to the output stream.                    */
				/*          Write the following sequence of bytes: M4_MARKER|1,0,0       */
				/*                                                                       */
				/* Case 4:  Matches were found And t >= 19 bytes                         */
				/*          t=t-18                                                       */
				/*          Write 0x00 on the output stream for each time 255 divides    */
				/*          evenly into the new value of t, and subtract 255 from t.     */
				/*			Write 17+t to the output stream.                             */
				/*          Write "t" litterals to the output stream.                    */
				/*          Write the following sequence of bytes: M4MRK|1,0,0           */
				/*                                                                       */
				/*************************************************************************/
				if (litteralLength > 0)
				{

					if ( (op == out) && (litteralLength <= 238) )
						*op++ = (17 + litteralLength);
					else if (litteralLength <= 3)
						op[-2] |= litteralLength;
					else if (litteralLength <= 18)
						*op++ = (litteralLength - 3);
					else
					{
						unsigned int tt = litteralLength - 18;

						*op++ = 0;
						while (tt > 255)
						{
							tt -= 255;

							/* prevent the compiler from transforming this loop
								* into a memset() call */
							* (volatile unsigned char *) op++ = 0;
						}
						*op++ = tt;
					}
					do *op++ = *initialIndex++; while (--litteralLength > 0);
				}

				*op++ = /*M4_MARKER*/0x10 | 1;
				*op++ = 0;
				*op++ = 0;

				/* Update the # of bytes on the output stream */
				*out_len = op - out;

				/* Give Semaphore acknowledging copy completed */
//				ReleaseSemaphore(*cpyRespSem,1,NULL);
			}
			break;

			case EXIT:
				/* FINISHED */
				printf("G_Matches=%d\n",G_Matches);
				ReleaseSemaphore(*cpyRespSem,1,NULL);
				ExitThread(0);
				break;

			default:
				printf("Should not get here.\n");
				continue;
		}
		/* Increment Command Array Index for next time */
		/* Check to increment or reset the "Queue" arrays */
		if((unsigned int)(LCL_pcommand) < MAX_CMD_BYTE_ADDR)
		{
			LCL_pcommand++;
			LCL_pstartOffset++;
			LCL_plitteralLength++;
			LCL_pmatchOffset++;
			LCL_pmatchLength++;
		}
		else
		{
			LCL_pcommand = &command[0];
			LCL_pstartOffset = &startOffset[0];
			LCL_plitteralLength = &litteralLength[0];
			LCL_pmatchOffset = &matchOffset[0];
			LCL_pmatchLength = &matchLengthArray[0];
		}
	}

	return 0;
}



#endif





#if 0
	/* Take over the system */
//	SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
//lock the thread to a certain core / cpu, try SetThreadAffinityMask.
//	SetThreadAffinityMask(GetCurrentThread(),0x2);	//2nd param is mask of cpus the sw can run on
	/* Give the system back */
//	SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
//CMPXCHG	/* Compare and Exchange.  Can execute atomically */

#endif




