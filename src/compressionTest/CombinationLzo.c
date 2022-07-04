/*******************************************************************************/
/* Combination LZO.c - Combines all of the speedups:  Multicore, Memcpy/memset library speedup,
/*                     Aligned Cache accesses.  */
/*******************************************************************************************/
#include <stdio.h>
#include <memory.h>
#include <intrin.h>
#include <xmmintrin.h>
#include "combinationLzo.h"


extern void * A_memcpy (void * dest, const void * src, size_t count); // Copy count bytes from src to dest
extern void   SetMemcpyCacheLimit(size_t);                            // Change limit in GetMemcpyCacheLimit
void * A_memset (void * dest, int c, size_t count);            // Set count bytes in dest to (char)c


//#define DBG
#undef DBG

#define LZO_HASH_VALUE		0x1824429D
#define lzo_uint	unsigned int
#define lzo_uint32	unsigned int
#define LZO_BYTE	(unsigned char)
#define UA_GET32(a)		*((unsigned int*)(a))
#define UA_COPY32(a,b)	*((unsigned int*)(a)) = *((unsigned int*)(b))
#define CHAR_BIT	8

#define M2_MAX_LEN		8
#define M2_MAX_OFFSET	0x800
#define M3_MAX_OFFSET	0x4000

#define M3_MARKER	0x20
#define M4_MARKER	0x10
#define M3_MAX_LEN	33
#define M4_MAX_LEN	0x9

#define LZO_MIN(a,b)        ((a) <= (b) ? (a) : (b))


/* Status Definitions */
#define STATUS_IDLE						0
#define STATUS_COMPRESS					1
#define STATUS_TERMINATE				2
#define STATUS_REASSEMBLE				3
#define STATUS_INIT						4
#define STATUS_ENDED					5
#define STATUS_REASSEMBLY_COMPLETE		6
#define STATUS_REASSEMBLY_FAILED		7
#define STATUS_REPORT_WHEN_DONE			8
#define STATUS_INIT_COMPLETE			9


static DWORD WINAPI multiCore_compress_thread(LPVOID lpParam);
static DWORD WINAPI reassembleDataThread(LPVOID lpParam);
static int lzo1x_1_15_compress_multiThread( const unsigned char* in, unsigned int  in_len,
                         unsigned char* out, unsigned int* out_len,
                         void* wrkmem );
static unsigned int
do_compress ( const unsigned char* in , unsigned int  in_len,
                    unsigned char* out, unsigned int* out_len,
                    unsigned int  ti,  void* wrkmem);



static void freeResources()
{
	/* TBD */

	return;
}




/*****************************************************************************/
/* initMultiCoreLZO - Function to initialize multi-threaded version of the   */
/*                    LZO 1x_1_15 compression routine.                       */
/*                                                                           */
/* Input Parameters should be passed by reference.                           */
/*****************************************************************************/
int initComboLZO(
	int action, 
	unsigned int numThreads,
	threadCommsStruct** instThreadComms, 
	unsigned char** instDictMem,
	HANDLE** instNodeHandle,
	HANDLE** instTaskCmpleteHandle,
	tmpOutBufStruct** instTmpBuffer,
	unsigned int numTmpOutputBuffers, 
	unsigned int tmpOutputBufferSizeBytes,
	reassembleType** reassembleInst)
{
	int x;
	threadCommsStruct *threadComms = NULL;  /* Pointer to Thread Communication Data Structure */
	unsigned char* dictMem = NULL;		    /* Pointer to Start of Dictionaries */
	HANDLE* NodeHandle = NULL;			    /* Array of Thread Handles */
	tmpOutBufStruct* tmpOutBufArray = NULL;	/* Array of Tmp Output Buffers */
	HANDLE* hSemTaskCmplete = NULL;
	reassembleType* reassembleInfo = NULL;

	if(action == INITIALIZE_LZO)
	{
		/* Init Return Parameters */
		*instThreadComms = NULL;
		*instDictMem = NULL;
		*instNodeHandle = NULL;
		*instTmpBuffer = NULL;
		*instTaskCmpleteHandle = NULL;
		*reassembleInst = NULL;

		/* Allocate Dictionary Resources */ 
		dictMem = NULL;		
		dictMem = (unsigned char*)_aligned_malloc(32*1024*numThreads,16);	/* Each dictionary has size 32kB */
		if(dictMem == NULL)
		{
			printf("Error allocating memory for dictionary.\n");
			freeResources();
			return -1;
		}

		/* Allocate Storage for temporary buffers */
		tmpOutBufArray = (tmpOutBufStruct*)malloc(sizeof(tmpOutBufStruct) * numTmpOutputBuffers);
		if(tmpOutBufArray == NULL)
		{
			printf("Error allocating space for temp buffer data structures.\n");
			freeResources();
			return -1;
		}
		for(x = 0; x < (int)numTmpOutputBuffers; x++)
		{
			tmpOutBufArray[x].buffer = (unsigned char*)malloc(tmpOutputBufferSizeBytes);
			if(tmpOutBufArray[x].buffer == NULL)
			{
				printf("Error allocating memory for temporary buffers");
				freeResources();
				return -1;
			}
			tmpOutBufArray[x].bufferMaxSizeBytes = tmpOutputBufferSizeBytes;
			tmpOutBufArray[x].bufferUsedSizeBytes = 0;
			tmpOutBufArray[x].blockNum = -1;
			tmpOutBufArray[x].id = x;
		}

		/* Allocate Handle Array used for Thread Task Completion */
		/* Note: 1 extra handle for reassemble task */
		hSemTaskCmplete = (HANDLE*)malloc(sizeof(HANDLE)*(numThreads+1));
		if(hSemTaskCmplete == NULL)
		{
			printf("Error allocating space for hSemTaskCmplete.\n");
			freeResources();
			return -1;
		}
		for(x = 0; x < (int)(numThreads+1); x++)
		{
			hSemTaskCmplete[x] = NULL;
		}

		/* Allocate 1 Handle for Each Thread that will operate on a block of data */
		/* Note: 1 extra handle for reassemble task */
		NodeHandle = (HANDLE*)malloc(sizeof(HANDLE) * (numThreads+1));
		if(NodeHandle == NULL)
		{
			printf("Error allocating space for thread handles.\n");
			freeResources();
			return -1;
		}
		for(x = 0; x < (int)(numThreads+1); x++)
		{
			NodeHandle[x] = NULL;
		}

		/* Allocate Reassemble Data Structure */
		reassembleInfo = (reassembleType*)malloc(sizeof(reassembleType));
		if(reassembleInfo == NULL)
		{
			printf("Error allocating space for reassemble data structure.\n");
			freeResources();
			return -1;
		}
		reassembleInfo->pOutput = NULL;
		reassembleInfo->action = 0;
		reassembleInfo->numBlocksCopied = 0;
		reassembleInfo->numBlocksToCopy = 0;
		reassembleInfo->numFreedTbuf = 0;
		reassembleInfo->numSavedTbuf = 0;
		reassembleInfo->hSemTaskCmplete = &hSemTaskCmplete[numThreads];

		/* Critical Shared Access Semaphore */
		reassembleInfo->semCritAccess = NULL;
		reassembleInfo->semCritAccess = CreateSemaphore( 
				NULL,           // default security attributes
				1,              // initial count
				1,				// maximum count
				NULL			// unnamed semaphore
			);
		if(reassembleInfo->semCritAccess == NULL)
		{
			printf("Error creating semaphore for Reassemble Critical Access.\n");
			freeResources();
			return -1;
		}
 	
		/* Reassemble Command Semaphore */
		reassembleInfo->hSemReassemble = NULL;
		reassembleInfo->hSemReassemble = CreateSemaphore( 
				NULL,           // default security attributes
				0,              // initial count
				1,				// maximum count
				NULL			// unnamed semaphore
			);
		if(reassembleInfo->hSemReassemble == NULL)
		{
			printf("Error creating semaphore for Reassemble Cmd.\n");
			freeResources();
			return -1;
		}

		/* Allocate Temp Buffers to Be Freed Array */
		reassembleInfo->freedTbufArray = NULL;
		reassembleInfo->freedTbufArray = (int*)malloc(sizeof(int)*numTmpOutputBuffers);
		if(reassembleInfo->freedTbufArray == NULL)
		{
			printf("Error allocating memory for temp buffer free array.\n");
			freeResources();
			return -1;
		}
		reassembleInfo->tmpfreedTbufArray = NULL;
		reassembleInfo->tmpfreedTbufArray = (int*)malloc(sizeof(int)*numTmpOutputBuffers);
		if(reassembleInfo->tmpfreedTbufArray == NULL)
		{
			printf("Error allocating memory for temp buffer free array.\n");
			freeResources();
			return -1;
		}

		/* Allocate Saved Temp Buffers Array */
		reassembleInfo->savedTbufArray = NULL;
		reassembleInfo->savedTbufArraySpare = NULL;
		/* Create An array of Saved temporary buffers         */
		/* Temporary Buffers are added to this array only if  */
		/* their data cannot be immediately copied out.       */
		reassembleInfo->savedTbufArray = (tmpOutBufStruct**)malloc(sizeof(tmpOutBufStruct*)*numTmpOutputBuffers);
		if(reassembleInfo->savedTbufArray == NULL)
		{
			printf("Error allocating memory for saved temp buffer array.\n");
			freeResources();
			return -1;
		}
		reassembleInfo->savedTbufArraySpare = (tmpOutBufStruct**)malloc(sizeof(tmpOutBufStruct*)*numTmpOutputBuffers);
		if(reassembleInfo->savedTbufArraySpare == NULL)
		{
			printf("Error allocating memory for spare saved temp buffer array.\n");
			freeResources();
			return -1;
		}

		/* Allocate Thread Communication Data Structures */
		threadComms = (threadCommsStruct*)malloc(sizeof(threadCommsStruct) * numThreads);
		if(threadComms == NULL)
		{
			printf("Error allocating space for thread communications data structures.\n");
			freeResources();
			return -1;
		}
		for(x = 0; x < (int)numThreads; x++)
		{
			threadComms[x].hSemCtrl = NULL;
		}

		/* Spawn the LZO Compression Threads */
		for(x = 0; x < (int)numThreads; x++)
		{
			/* Init Thread Data */
			threadComms[x].hSemCtrl = CreateSemaphore( 
				NULL,           // default security attributes
				0,              // initial count
				1,				// maximum count
				NULL			// unnamed semaphore
			);
			hSemTaskCmplete[x] = CreateSemaphore( 
				NULL,           // default security attributes
				0,              // initial count
				1,				// maximum count
				NULL			// unnamed semaphore
			);
			if( (threadComms[x].hSemCtrl == NULL) || (hSemTaskCmplete[x] == NULL) )
			{
				printf("Error creating semaphores for LZO Block Compression Thread #%d.\n",threadComms[x].threadId);
				freeResources();
				return -1;
			}
			threadComms[x].status = STATUS_IDLE;
			threadComms[x].threadId = x;
			threadComms[x].in = NULL;
			threadComms[x].in_len = 0;
			threadComms[x].tmpOutBuffer = NULL;
			threadComms[x].hSemTaskCmplete = &hSemTaskCmplete[x];
			threadComms[x].ptrDictionary = (unsigned short*)&(dictMem[x*(32*1024)]);

			/* Spawn Thread */
			NodeHandle[x] = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)multiCore_compress_thread,
				(LPVOID)&threadComms[x],0,NULL);
			if(NodeHandle[x] == NULL)
			{
				printf("Error in spawning of LZO Block Compression Thread #%d.\n",threadComms[x].threadId);
				freeResources();
				return -1;
			}
		}


		/* Spawn the Reassembly Thread */
		hSemTaskCmplete[numThreads] = CreateSemaphore( 
				NULL,           // default security attributes
				0,              // initial count
				1,				// maximum count
				NULL			// unnamed semaphore
			);
		if( hSemTaskCmplete[numThreads] == NULL )
		{
			printf("Error creating semaphores for Reassembly Task Completion.\n");
			freeResources();
			return -1;
		}
		NodeHandle[x] = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)reassembleDataThread,
				(LPVOID)reassembleInfo,0,NULL);
		if(NodeHandle[x] == NULL)
		{
			printf("Error in spawning of LZO Reassembly Thread.\n");
			freeResources();
			return -1;
		}

		/* Successful Initialization, Assign Return Values to Passed in Paramters */
		*instThreadComms = threadComms;				/* Pointer to Thread Communication Data Structure */
		*instDictMem = dictMem;						/* Pointer to Start of Dictionaries */
		*instNodeHandle = NodeHandle;				/* Array of Thread Handles */
		*instTmpBuffer = tmpOutBufArray;			/* Array of Tmp Output Buffers */
		*instTaskCmpleteHandle = hSemTaskCmplete;
		*reassembleInst = reassembleInfo;
	}
	else if(action == CLEANUP_LZO)
	{
		threadComms = *instThreadComms;				/* Pointer to Thread Communication Data Structure */
		dictMem = *instDictMem;						/* Pointer to Start of Dictionaries */
		NodeHandle = *instNodeHandle;				/* Array of Thread Handles */
		tmpOutBufArray = *instTmpBuffer;			/* Array of Tmp Output Buffers */
		hSemTaskCmplete = *instTaskCmpleteHandle;
		reassembleInfo = *reassembleInst;

		/**************************************************************/
		/* Only Cleanup if data was previously allocated successfully */
		/**************************************************************/
		

		/* Terminate Running Threads and Free Thread Handles */
		if(NodeHandle != NULL)
		{
			/* Send Termination Notice to existing threads & Cleanup Handles */
			for(x=0; x < (int)numThreads; x++)
			{
				if(NodeHandle[x] != NULL)
				{
					/* Signal Thread Terminate */
					threadComms[x].status = STATUS_TERMINATE;
					ReleaseSemaphore(threadComms[x].hSemCtrl,1,NULL);

					/* Wait for Thread to Terminate */
					if( WaitForSingleObject(hSemTaskCmplete[x],INFINITE) != WAIT_OBJECT_0)
					{
						printf("Error, Wait Failed for Response from Thread during Termination.\n");
					}
					if(threadComms[x].status == STATUS_TERMINATE)
					{
						printf("Error in termination of thread #%d, value =%d, expected %d\n",threadComms[x].threadId, threadComms[x].status,STATUS_ENDED);
					}			
					CloseHandle(NodeHandle[x]);
					NodeHandle[x] = NULL;
				}
			}

			/* Send Termination Notice to Reassembly Thread */
			if((reassembleInfo != NULL) && (NodeHandle[x] != NULL))
			{
				while( WaitForSingleObject(hSemTaskCmplete[x],0) == WAIT_OBJECT_0);
				reassembleInfo->action = STATUS_TERMINATE;
				ReleaseSemaphore(reassembleInfo->hSemReassemble,1,NULL);

				/* Wait for Thread to Terminate */
				if( WaitForSingleObject(hSemTaskCmplete[x],INFINITE) != WAIT_OBJECT_0)
				{
					printf("Error, Wait Failed for Response from Reassembly Thread during Termination.\n");
				}
				CloseHandle(NodeHandle[x]);
				NodeHandle[x] = NULL;

				/* Free Reassemble Resources */
				CloseHandle(reassembleInfo->semCritAccess);
				CloseHandle(reassembleInfo->hSemReassemble);
				if(reassembleInfo->freedTbufArray != NULL)
					free(reassembleInfo->freedTbufArray);
				if(reassembleInfo->tmpfreedTbufArray != NULL)
					free(reassembleInfo->tmpfreedTbufArray);
				if(reassembleInfo->savedTbufArray != NULL)
					free(reassembleInfo->savedTbufArray);
				if(reassembleInfo->savedTbufArraySpare != NULL)
					free(reassembleInfo->savedTbufArraySpare);
				free(reassembleInfo);
			}
			free(NodeHandle);
		}

		/* Free Thread Data */
		if(threadComms != NULL)
		{
			for(x=0; x < (int)numThreads; x++)
			{
				threadComms[x].hSemTaskCmplete = NULL;
				threadComms[x].ptrDictionary = NULL;
				if(threadComms[x].hSemCtrl != NULL)
				{
					CloseHandle(threadComms[x].hSemCtrl);
					threadComms[x].hSemCtrl = NULL;
				}
			}
			free(threadComms);
		}

		/* Free Temporary Output Buffers */
		if(tmpOutBufArray != NULL)
		{
			for(x = 0; x < (int)numTmpOutputBuffers; x++)
			{
				if(tmpOutBufArray[x].buffer != NULL)
				{
					free(tmpOutBufArray[x].buffer);
					tmpOutBufArray[x].buffer = NULL;
				}
			}
			free(tmpOutBufArray);
		}
		
		/* Free Complete Semaphores */
		if(hSemTaskCmplete != NULL)
		{
			for(x=0; x < (int)(numThreads+1); x++)
			{
				if(hSemTaskCmplete[x] != NULL)
				{
					CloseHandle(hSemTaskCmplete[x]);
					hSemTaskCmplete[x] = NULL;
				}
			}
			free(hSemTaskCmplete);
		}

		/* Free Dictionary Memory */
		if(dictMem != NULL)
		{
			_aligned_free(dictMem);
		}
		
		/* Update Return Values */
		*instThreadComms = NULL;
		*instDictMem = NULL;
		*instNodeHandle = NULL;
		*instTmpBuffer = NULL;
		*instTaskCmpleteHandle = NULL;
		*reassembleInst = NULL;
	}
	else
	{
		printf("Unknown Initialization Action.\n");
		return -1;
	}

	return 0;
}





int lzo1x_1_15_Combo_control(const unsigned char* in, unsigned int  in_len,
								 unsigned char* out, unsigned int* out_len,
								 int numLzoThreads, int blockSize, 
								 tmpOutBufStruct* tmpBufferArray,
								 unsigned int numTmpBuffers, 
								 HANDLE* hSemTaskCmplete,
								 threadCommsStruct* threadComms,
								 reassembleType* reassembleInfo)
{
	unsigned char* ptrInput, *ptrOutput;
	unsigned int numBlocks, nextBlock;
	int numInit,x, numFreeBuffers,numThreadsToReschedule, tmpBufferIndex;
	DWORD rval;
	int *threadRescheduleList = NULL;
	int *freeTbufArray = NULL;
	unsigned int bytesLeftToCompress = in_len;
	unsigned int numBlocksReceived = 0;
	ptrInput = (unsigned char*)in;
	ptrOutput = (unsigned char*)out;

	*out_len = 0;
	reassembleInfo->numSavedTbuf = 0;

	/* Sanity Checks */
	if(in_len <= 0)
	{
		printf("Error, Nothing to Compress.\n");
		return -1;
	}
	if(numTmpBuffers < (unsigned int)numLzoThreads)
	{
		printf("Error, Not enough temporary output buffers.\n");
		return -1;
	}

	/* Determine the # of blocks to be compressed */
	numBlocks = in_len / blockSize;
	if( (numBlocks*blockSize) < in_len )
		numBlocks++;

	/********************************/
	/* Initialize Reassembly Thread */
	/********************************/
	reassembleInfo->action = STATUS_INIT;
	reassembleInfo->pOutput = (unsigned char*)out;
	reassembleInfo->numBlocksToCopy = numBlocks;

	/* Ensure reassembly response is not signaled */
	while( WaitForSingleObject(hSemTaskCmplete[numLzoThreads],0) == WAIT_OBJECT_0);
	ReleaseSemaphore(reassembleInfo->hSemReassemble,1,NULL);
	
	/* Wait for reassembly thread initialization to complete */
	while(1)
	{
		if( WaitForSingleObject(hSemTaskCmplete[numLzoThreads],INFINITE) == WAIT_OBJECT_0)
		{
#ifdef DBG
			printf("Reassemble Thread Init Complete.\n");
#endif
			if(reassembleInfo->status == STATUS_INIT_COMPLETE)
				break;
			ReleaseSemaphore(reassembleInfo->hSemReassemble,1,NULL);
		}
		else
		{
			printf("Error occurred initializing reassembly thread.\n");
			return -1;
		}
	}
	reassembleInfo->action = STATUS_REASSEMBLE;	/* All further actions will be type reassemble */


	/************************************************************/
	/* Create and Initialize An array of free temporary buffers */
	/************************************************************/
	freeTbufArray = (int*)malloc(sizeof(int)*numTmpBuffers);
	if(freeTbufArray == NULL)
	{
		printf("Error allocating memory for temp buffer free array.\n");
		return -1;
	}
	for(x=0;x<(int)numTmpBuffers;x++)
	{
		freeTbufArray[x] = x;
	}
	numFreeBuffers = numTmpBuffers;
	reassembleInfo->numFreedTbuf = 0;	/* Reinit to 0 */


	/*************************************************************/
	/* List of threads to reschedule                             */
	/* This is populated when a thread needs to be halted due to */
	/* lack of free available temporary buffers.                 */
	/*************************************************************/
	threadRescheduleList = (int*)malloc(sizeof(int)*numLzoThreads);
	if(threadRescheduleList == NULL)
	{
		printf("Error allocating memory for thread reschedule list.\n");
		return -1;
	}
	numThreadsToReschedule = 0;


	/********************************************/
	/* Assign initial tasking and start threads */
	/********************************************/
	if(numBlocks < (unsigned int)numLzoThreads)
		numInit = numBlocks;
	else
		numInit = numLzoThreads;

	for(x = 0; x < numInit; x++)
	{
		threadComms[x].status = STATUS_COMPRESS;
		tmpBufferIndex = freeTbufArray[(numFreeBuffers-1)];
		tmpBufferArray[tmpBufferIndex].blockNum = x;
		threadComms[x].tmpOutBuffer = &tmpBufferArray[tmpBufferIndex];
#ifdef DBG
		printf("Initial Thread using buffer %d\n",tmpBufferIndex);
		freeTbufArray[(numFreeBuffers-1)] = -1;
#endif
		threadComms[x].in = ptrInput;
		if(bytesLeftToCompress > (unsigned int)blockSize)
		{
			threadComms[x].in_len = blockSize;
			bytesLeftToCompress -= blockSize;
		}
		else
		{
			threadComms[x].in_len = bytesLeftToCompress;
			bytesLeftToCompress = 0;
		}
		ReleaseSemaphore(threadComms[x].hSemCtrl,1,NULL);

		ptrInput += blockSize;
		numFreeBuffers--;
	}
	nextBlock = x;
	

	/*************************************************************/
	/* Main Loop - Thread FIFO for assigning compression tasking */
	/*************************************************************/
	while(1)
	{
		/* Check to see if there are any more blocks left to assign for compression */
		if(bytesLeftToCompress <= 0)
		{
			break;
		}

		/* Wait for any thread to respond */
		rval = WaitForMultipleObjects(numLzoThreads+1,hSemTaskCmplete,FALSE,INFINITE);
		if( (rval >= WAIT_OBJECT_0) && (rval != (WAIT_OBJECT_0+numLzoThreads)) )
		{
			rval -= WAIT_OBJECT_0;	/* Index of signaled object */
		}
		else if(rval == WAIT_TIMEOUT)
		{
			printf("Error Timeout Occurred After Calling WaitForMultipleObjects.\n");
			continue;
		}
		else if(rval == (WAIT_OBJECT_0+numLzoThreads))
		{
			/************************************************/
			/* Reassemble Thread Signaled Back To Ctrl Task */
			/************************************************/
			/* GET Shared LOCK */
			if(WaitForSingleObject(reassembleInfo->semCritAccess,INFINITE) != WAIT_OBJECT_0)
			{
				printf("Error, Wait Failed in Ctrl Thread for Critical Semapore.\n");
				continue;
			}
			if(reassembleInfo->numFreedTbuf > 0)
			{
				/* Update free buffer array and number of free buffers */
				memcpy(&freeTbufArray[numFreeBuffers], reassembleInfo->freedTbufArray,reassembleInfo->numFreedTbuf*sizeof(int));
				numFreeBuffers += reassembleInfo->numFreedTbuf;
#ifdef DBG
				printf("Reassemble Returned and Freed %d buffers\n",reassembleInfo->numFreedTbuf);
				printf("numFreed tbuf now = %d\n",reassembleInfo->numFreedTbuf);

				/* Print Free Blocks */
				{
					int y;
					printf("\n\nFreed Updated\n");
					for(y=0;y<numFreeBuffers;y++)
					{
						printf("%d ",freeTbufArray[y]);
					}
					printf("\n\n");
				}
#endif
				reassembleInfo->numFreedTbuf = 0;
			}
			/* RELEASE LOCK */
			ReleaseSemaphore(reassembleInfo->semCritAccess,1,NULL);

			/********************************************************************************************************/
			/* If any threads need to be rescheduled and temporary buffers have become available, allow them to run */
			/********************************************************************************************************/
			while( (numFreeBuffers > 0) && (numThreadsToReschedule > 0) )
			{
				/* If there is no more data left to compress, exit */
				if(bytesLeftToCompress <= 0)
				{
					break;
				}

				/* Give the Idle Thread the next block to compress */
				rval = threadRescheduleList[numThreadsToReschedule-1];
				threadComms[rval].status = STATUS_COMPRESS;
				tmpBufferIndex = freeTbufArray[(numFreeBuffers-1)];
				tmpBufferArray[tmpBufferIndex].blockNum = nextBlock++;
				threadComms[rval].tmpOutBuffer = &tmpBufferArray[tmpBufferIndex];
#ifdef DBG
				{
					int y;
					printf("\n\n***Ctrl Thread Free Array: ");
					for(y=0; y < numFreeBuffers;y++)
						printf("%d ",freeTbufArray[y]);
					printf("\n\n");
				}
				printf("Resumed Thread using buffer %d\n",tmpBufferIndex);
				freeTbufArray[(numFreeBuffers-1)] = -1;
#endif
				threadComms[rval].in = ptrInput;
				if(bytesLeftToCompress > (unsigned int)blockSize)
				{
					threadComms[rval].in_len = blockSize;
					bytesLeftToCompress -= blockSize;
				}
				else
				{
					threadComms[rval].in_len = bytesLeftToCompress;
					bytesLeftToCompress = 0;
				}
				ReleaseSemaphore(threadComms[rval].hSemCtrl,1,NULL);

				ptrInput += blockSize;
				numFreeBuffers--;
				numThreadsToReschedule--;
			}
			continue;
		}
		else
		{
			printf("Error %u Occurred After Calling WaitForMultipleObjects, rval = %u.\n",GetLastError(),rval);
			continue;
		}


		/* A thread responded, update the output size & increment # of compressed blocks */
		*out_len += threadComms[rval].tmpOutBuffer->bufferUsedSizeBytes;
		numBlocksReceived++;

		/* Add the temporary buffer to the saved list and signal the reassembly thread */
		/* GET Shared LOCK */
		if(WaitForSingleObject(reassembleInfo->semCritAccess,INFINITE) != WAIT_OBJECT_0)
		{
			printf("Error, Wait Failed in Ctrl Thread for Critical Semapore.\n");
			continue;
		}
		reassembleInfo->savedTbufArray[reassembleInfo->numSavedTbuf] = threadComms[rval].tmpOutBuffer;
		reassembleInfo->numSavedTbuf++;
#ifdef DBG
		printf("Thread %d Returned From Compressing Data (block %d).  Data @ 0x%X.\n\tAdded saved tbuf for ID %d.  Num saved tbuf now %d\n",rval,
			reassembleInfo->savedTbufArray[reassembleInfo->numSavedTbuf-1]->blockNum,(unsigned int)reassembleInfo->savedTbufArray[reassembleInfo->numSavedTbuf-1],
			reassembleInfo->savedTbufArray[reassembleInfo->numSavedTbuf-1]->id,reassembleInfo->numSavedTbuf);
#endif
		/* RELEASE LOCK */
		ReleaseSemaphore(reassembleInfo->semCritAccess,1,NULL);
		ReleaseSemaphore(reassembleInfo->hSemReassemble,1,NULL);  /* SIGNAL REASSEMBLE THREAD */

		/****************************************************************************************/
		/* If available temp buffers exist, give the now Idle Thread the next block to compress */
		/* Otherwise, put the thread in the reschedule list.                                    */
		/****************************************************************************************/
		if(numFreeBuffers > 0)
		{
			threadComms[rval].status = STATUS_COMPRESS;
			tmpBufferIndex = freeTbufArray[(numFreeBuffers-1)];
			tmpBufferArray[tmpBufferIndex].blockNum = nextBlock++;
			threadComms[rval].tmpOutBuffer = &tmpBufferArray[tmpBufferIndex];
#ifdef DBG
			{
				int y;
				printf("\n\n***Ctrl Thread Free Array: ");
				for(y=0; y < numFreeBuffers;y++)
					printf("%d ",freeTbufArray[y]);
				printf("\n\n");
			}
			printf("Thread using buffer %d\n",tmpBufferIndex);
			freeTbufArray[(numFreeBuffers-1)] = -1;
#endif

			threadComms[rval].in = ptrInput;
			if(bytesLeftToCompress > (unsigned int)blockSize)
			{
				threadComms[rval].in_len = blockSize;
				bytesLeftToCompress -= blockSize;
			}
			else
			{
				threadComms[rval].in_len = bytesLeftToCompress;
				bytesLeftToCompress = 0;
			}
			ptrInput += blockSize;
			ReleaseSemaphore(threadComms[rval].hSemCtrl,1,NULL);
			numFreeBuffers--;
		}
		else
		{
			/* No free buffers, add thread to the reschedule list */
			threadRescheduleList[numThreadsToReschedule] = rval;
			numThreadsToReschedule++;
		}
	}


	/***************************************************/
	/* Wait for remaining blocks to finish compression */
	/***************************************************/
	while(numBlocksReceived != numBlocks)
	{
		/* Wait for any thread to respond */
		rval = WaitForMultipleObjects(numLzoThreads+1,hSemTaskCmplete,FALSE,INFINITE);
		if( (rval >= WAIT_OBJECT_0) && (rval != (WAIT_OBJECT_0+numLzoThreads)) )
		{
			rval -= WAIT_OBJECT_0;	/* Index of signaled object */
		}
		else if(rval == WAIT_TIMEOUT)
		{
			printf("Error Timeout Occurred After Calling WaitForMultipleObjects.\n");
			continue;
		}
		else if(rval == (WAIT_OBJECT_0+numLzoThreads))
		{
			/* Reassemble Thread, nothing to be done now */
			continue;
		}
		else
		{
			printf("Error %u Occurred After Calling WaitForMultipleObjects, rval = %u.\n",GetLastError(),rval);
			continue;
		}

		/* A thread responded, increment the # of compressed blocks */
		numBlocksReceived++;
		*out_len += threadComms[rval].tmpOutBuffer->bufferUsedSizeBytes;

		/* Add the temporary buffer to the saved list and signal the reassembly thread */
		/* GET Shared LOCK */
		if(WaitForSingleObject(reassembleInfo->semCritAccess,INFINITE) != WAIT_OBJECT_0)
		{
			printf("Error, Wait Failed in Ctrl Thread for Critical Semapore.\n");
			continue;
		}
		reassembleInfo->savedTbufArray[reassembleInfo->numSavedTbuf] = threadComms[rval].tmpOutBuffer;
		reassembleInfo->numSavedTbuf++;
#ifdef DBG
		printf("Remaining Block Loop:  Thread %d Returned From Compressing Data (block %d).  Data @ 0x%X.\n\tAdded saved tbuf for ID %d.  Num saved tbuf now %d\n",rval,
			reassembleInfo->savedTbufArray[reassembleInfo->numSavedTbuf-1]->blockNum,(unsigned int)reassembleInfo->savedTbufArray[reassembleInfo->numSavedTbuf-1],
			reassembleInfo->savedTbufArray[reassembleInfo->numSavedTbuf-1]->id,reassembleInfo->numSavedTbuf);
#endif
		/* RELEASE LOCK */
		ReleaseSemaphore(reassembleInfo->semCritAccess,1,NULL);
		ReleaseSemaphore(reassembleInfo->hSemReassemble,1,NULL);  /* SIGNAL REASSEMBLE THREAD */
	}

#ifdef DBG
	printf("\n\nAll data is now compressed.\n\n");
#endif

	/* Wait for Reassembly Thread to Complete Execution */
	reassembleInfo->action = STATUS_REPORT_WHEN_DONE;
	ReleaseSemaphore(reassembleInfo->hSemReassemble,1,NULL);
	while(1)
	{
		/* Wait for any thread to respond */
		rval = WaitForMultipleObjects(numLzoThreads+1,hSemTaskCmplete,FALSE,INFINITE);
		if( (rval >= WAIT_OBJECT_0) && (rval != (WAIT_OBJECT_0+numLzoThreads)) )
		{
			rval -= WAIT_OBJECT_0;	/* Index of signaled object */
		}
		else if(rval == WAIT_TIMEOUT)
		{
			printf("Error Timeout Occurred After Calling WaitForMultipleObjects.\n");
			continue;
		}
		else if(rval == (WAIT_OBJECT_0+numLzoThreads))
		{
			/* Reassemble Thread Completed, Check to see if its done */
			if(reassembleInfo->status == STATUS_REASSEMBLY_COMPLETE)
			{
#ifdef DBG
				printf("***DONE***\n");
#endif
				break;
			}
			else
			{
#ifdef DBG
				printf("Retry\n");
				ReleaseSemaphore(reassembleInfo->hSemReassemble,1,NULL);
#endif
			}

		}
		else
		{
			printf("Error %u Occurred After Calling WaitForMultipleObjects, rval = %u.\n",GetLastError(),rval);
			continue;
		}
	}

	/* Cleanup */
	free(freeTbufArray);
	free(threadRescheduleList);

	return 0;
}



static DWORD WINAPI reassembleDataThread(LPVOID lpParam)
{

	reassembleType  localReassembleInfo;
	unsigned int numCopied;
	reassembleType* reassembleInfo = (reassembleType*)lpParam;
	unsigned int nextBlockToCopy = 0;
	unsigned char* ptrOutput = NULL;
	int x,y;
	int respond;

	while(1)
	{
		/* Wait for Go Signal */
		if(WaitForSingleObject(reassembleInfo->hSemReassemble,INFINITE) != WAIT_OBJECT_0)
		{
			printf("Error, Wait Failed in Reassemble Thread.\n");
			continue;
		}

		/* Only respond back if required */
		respond = 0;

		if( (reassembleInfo->action == STATUS_REASSEMBLE) ||
			(reassembleInfo->action == STATUS_REPORT_WHEN_DONE) )
		{
			/* Get Exclusive Access to the Shared Data */
			if(WaitForSingleObject(reassembleInfo->semCritAccess,INFINITE) != WAIT_OBJECT_0)
			{
				printf("Error, Wait For Critical Access Semaphore Failed in Reassemble Thread\n");
				continue;
			}

			/* Add new saved buffers from the Ctrl Thread to the reassemble thread's save buffer */
			for(y=0;y<reassembleInfo->numSavedTbuf;y++)
			{
				localReassembleInfo.savedTbufArraySpare[localReassembleInfo.numSavedTbuf++] = reassembleInfo->savedTbufArray[y];
			}
			localReassembleInfo.numFreedTbuf = 0;
			reassembleInfo->numSavedTbuf = 0;	/* Erase Saved Array */

#ifdef DBG	
			/* Entering, Print Saved Blocks */
			printf("\n\nSaved Enter\n");
			for(y=0;y<localReassembleInfo.numSavedTbuf;y++)
			{
				printf("%d (blk%d) ",localReassembleInfo.savedTbufArraySpare[y]->id, 
					localReassembleInfo.savedTbufArraySpare[y]->blockNum);
			}
			printf("\n\n");
#endif
			ReleaseSemaphore(reassembleInfo->semCritAccess,1,NULL);


			/* Attempt to Reassemble the Data */
			while(localReassembleInfo.numSavedTbuf > 0)
			{
				numCopied = 0;
				for(x = 0; x < localReassembleInfo.numSavedTbuf; x++)
				{
					if(localReassembleInfo.savedTbufArraySpare[x]->blockNum == nextBlockToCopy)
					{
						/* Copy the data to the output buffer */
						memcpy(ptrOutput,localReassembleInfo.savedTbufArraySpare[x]->buffer,
							localReassembleInfo.savedTbufArraySpare[x]->bufferUsedSizeBytes);
						ptrOutput+=localReassembleInfo.savedTbufArraySpare[x]->bufferUsedSizeBytes;
#ifdef DBG
						printf("Middle Freed %d (blk%d), Total=%d\n",localReassembleInfo.savedTbufArraySpare[x]->id,
							localReassembleInfo.savedTbufArraySpare[x]->blockNum,localReassembleInfo.numFreedTbuf+1);
#endif
						/* Free up the temporary buffer and increase the # of free available buffers */
						localReassembleInfo.tmpfreedTbufArray [localReassembleInfo.numFreedTbuf] = localReassembleInfo.savedTbufArraySpare[x]->id;
						localReassembleInfo.numFreedTbuf++;
						numCopied++;
						nextBlockToCopy++;

						/* Re-adjust saved tbuf array so there are no gaps */
						memmove(&localReassembleInfo.savedTbufArraySpare[x], &localReassembleInfo.savedTbufArraySpare[x+1], 
							sizeof(tmpOutBufStruct*)*(localReassembleInfo.numSavedTbuf-(x+1)) );
						
						localReassembleInfo.numSavedTbuf--;
						x--;
					}
#if 0
					else if( localReassembleInfo.savedTbufArraySpare[x]->blockNum < nextBlockToCopy)
					{
						printf("\nTHIS SHOULD NOT BE POSSIBLE\n");
					}
#endif
				}

				if(numCopied == 0)
				{
					break;
				}
				else
				{
					/* Update the # of copied blocks */
					localReassembleInfo.numBlocksCopied += numCopied;

					/* Respond to the Control Thread if at least 1 block was reassembled */
					respond = 1;
				}
			}


			/**************************************************************************/
			/* Update shared data structures, Get Exclusive Access to the Shared Data */
			/**************************************************************************/
			if(WaitForSingleObject(reassembleInfo->semCritAccess,INFINITE) != WAIT_OBJECT_0)
			{
				printf("Error, Wait For Critical Access Semaphore Failed in Reassemble Thread\n");
				continue;
			}

			memcpy(&reassembleInfo->freedTbufArray[reassembleInfo->numFreedTbuf],localReassembleInfo.tmpfreedTbufArray,localReassembleInfo.numFreedTbuf*sizeof(int));
			reassembleInfo->numFreedTbuf += localReassembleInfo.numFreedTbuf;
#ifdef DBG
			printf("Reassemble Freed %d buffers, total free = %d\n",localReassembleInfo.numFreedTbuf,reassembleInfo->numFreedTbuf);
#endif
			localReassembleInfo.numFreedTbuf = 0;
			reassembleInfo->numBlocksCopied = localReassembleInfo.numBlocksCopied;

#ifdef DBG
			/* Exiting, Print Saved Blocks */
			printf("\n\nSaved Exit\n");
			for(y=0;y<localReassembleInfo.numSavedTbuf;y++)
			{
				printf("%d (blk%d) ",localReassembleInfo.savedTbufArraySpare[y]->id, 
					localReassembleInfo.savedTbufArraySpare[y]->blockNum);
			}
			printf("\n\n");
			printf("Next block to copy is %d\n\n",nextBlockToCopy);
#endif
			/* Modify Response if REPORT_WHEN_DONE */
			if(reassembleInfo->action == STATUS_REPORT_WHEN_DONE)
			{
				/* Always respond to Ctrl Thread for this action */
				respond = 1;
				if( (reassembleInfo->numBlocksCopied != reassembleInfo->numBlocksToCopy) ||
					(reassembleInfo->numSavedTbuf != 0) )
				{
					reassembleInfo->status = STATUS_REASSEMBLY_FAILED;
				}
				else
				{
					reassembleInfo->action = STATUS_IDLE;
					reassembleInfo->status = STATUS_REASSEMBLY_COMPLETE;
				}
			}
			ReleaseSemaphore(reassembleInfo->semCritAccess,1,NULL);

			/* Respond to Ctrl Thread if buffers were freed, or reassembly complete */
			if(respond > 0)
			{
				ReleaseSemaphore(*(reassembleInfo->hSemTaskCmplete),1,NULL);
			}
		}
		else if(reassembleInfo->action == STATUS_INIT)
		{
			memcpy(&localReassembleInfo,reassembleInfo,sizeof(reassembleType));
			respond = 0;
			reassembleInfo->numBlocksCopied = 0;
			nextBlockToCopy = 0;
			ptrOutput = reassembleInfo->pOutput;
			localReassembleInfo.numBlocksCopied= 0;
			localReassembleInfo.numSavedTbuf = 0;
			localReassembleInfo.numFreedTbuf = 0;
			reassembleInfo->action = STATUS_IDLE;
			reassembleInfo->status = STATUS_INIT_COMPLETE;
			ReleaseSemaphore(*(reassembleInfo->hSemTaskCmplete),1,NULL);
		}
		else if(reassembleInfo->action == STATUS_TERMINATE)
		{
			reassembleInfo->action = STATUS_ENDED;
			ReleaseSemaphore(*(reassembleInfo->hSemTaskCmplete),1,NULL);
			ExitThread(0);
		}
		else if(reassembleInfo->action == STATUS_IDLE)
		{
			continue;
		}
		else
		{
			printf("Error, Unknown Command %d occurred during compression in reassemble thread.\n", 
				reassembleInfo->action);
		}
	}

	return 0;
}




/*****************************************************************************/
/* multiCore_compress_thread - Main Thread routine for compression threads.  */
/*****************************************************************************/
static DWORD WINAPI multiCore_compress_thread(LPVOID lpParam)
{
	threadCommsStruct* threadSharedData = (threadCommsStruct*)lpParam;

	/* Loop Waiting to Receive Compression or Terminate Commands */
	/* From the Control Thread                                   */
	while(1)
	{
		/* Wait for Go Signal */
		if(WaitForSingleObject(threadSharedData->hSemCtrl,INFINITE) != WAIT_OBJECT_0)
		{
			printf("Error, Wait Failed in multiCore_compress_thread.  Thread #%u\n",threadSharedData->threadId);
		}

		/* Determine Action to Perform and Perform It */
		if(threadSharedData->status == STATUS_COMPRESS)
		{
			if( lzo1x_1_15_compress_multiThread(threadSharedData->in, 
												threadSharedData->in_len,
												threadSharedData->tmpOutBuffer->buffer,
												&threadSharedData->tmpOutBuffer->bufferUsedSizeBytes,
												threadSharedData->ptrDictionary) != 0)
			{
				printf("Error occurred during compression in thread %u\n", threadSharedData->threadId);
			}
			threadSharedData->status = STATUS_IDLE;
			ReleaseSemaphore(*(threadSharedData->hSemTaskCmplete),1,NULL);
		}
		else if(threadSharedData->status == STATUS_TERMINATE)
		{
			threadSharedData->status = STATUS_ENDED;
			ReleaseSemaphore(*(threadSharedData->hSemTaskCmplete),1,NULL);
			ExitThread(0);
		}
		else
		{
			printf("Error, Unknown Command %d occurred during compression in thread %u\n", 
				threadSharedData->status, threadSharedData->threadId);
		}
	}

	return 0;
}




/*****************************************************************************/
/* lzo1x_1_15_compress_multiThread - LZO 1x_1_15 Compression Routine         */
/*****************************************************************************/
static int lzo1x_1_15_compress_multiThread( const unsigned char* in, unsigned int  in_len,
                         unsigned char* out, unsigned int* out_len,
                         void* wrkmem )
{
	const unsigned char* ip = in;
	unsigned char* op = out;
    unsigned int l = in_len;
    unsigned int t = 0;

    while (l > 20)
    {
        unsigned int ll = l;
        unsigned int ll_end;

        ll = LZO_MIN(ll, 49152 );
        ll_end = ((unsigned int)ip + ll);
        if ((ll_end + ((t + ll) >> 5)) <= ll_end || (const unsigned char*)(ll_end + ((t + ll) >> 5)) <= ip + ll)
            break;

//        memset(wrkmem, 0, ((unsigned int)1 << 13 /*DBITS*/) * sizeof(unsigned short));
		/* Faster Memset */
		A_memset(wrkmem, 0, ((unsigned int)1 << 13) * sizeof(unsigned short) );

        t = do_compress(ip,ll,op,out_len,t,wrkmem);
        ip += ll;
        op += *out_len;
        l  -= ll;
    }
    t += l;

    if (t > 0)
    {
        const unsigned char* ii = in + in_len - t;

        if (op == out && t <= 238)
            *op++ = (unsigned char)(17 + t);
        else if (t <= 3)
            op[-2] |= (unsigned char)(t);
        else if (t <= 18)
            *op++ = (unsigned char)(t - 3);
        else
        {
            unsigned int tt = t - 18;
            *op++ = 0;
            while (tt > 255)
            {
                tt -= 255;

                /* prevent the compiler from transforming this loop
                 * into a memset() call */
                * (volatile unsigned char *) op++ = 0;
            }
            *op++ = (unsigned char)(tt);
        }

		/* KANE-Why isnt this in the faster memcpy version? */
#if 1
		A_memcpy(op,ii,t);
		op += t;
		ii += t;
#endif
//        do *op++ = *ii++; while (--t > 0);
    }

    *op++ = M4_MARKER | 1;
    *op++ = 0;
    *op++ = 0;

    *out_len = op - out; //pd(op, out);

	return 0; //LZO_E_OK;
}




/***********************************************************************
// compress a block of data.
************************************************************************/
static unsigned int
do_compress ( const unsigned char* in , unsigned int  in_len,
                    unsigned char* out, unsigned int* out_len,
                    unsigned int  ti,  void* wrkmem)
{
    register const unsigned char* ip;
    unsigned char* op;
    const unsigned char* const in_end = in + in_len;
    const unsigned char* const ip_end = in + in_len - 20;
    const unsigned char* ii;
    unsigned short* const dict = (unsigned short*) wrkmem;

    op = out;
    ip = in;
    ii = ip;

	//Removed for aligned cache accesses
//    ip += ti < 4 ? 4 - ti : 0;

    for (;;)
    {
        const unsigned char* m_pos;

        lzo_uint m_off;
        lzo_uint m_len;
        {
			lzo_uint32 dv;
			lzo_uint dindex;
literal:
#if 0
	        ip += 1 + ((ip - ii) >> 5);
#endif
#if 1
			/* Code for Cache Aligned Accesses */
			m_off = ip-ii;
			if(m_off < 32)
			{
				ip += 4;
			}
			else
			{
				ip += 4 + (m_off>>5) & 0xFFFFFFFC; 
			}

#endif

next:
			if (ip >= ip_end)
	            break;
	        dv = UA_GET32(ip);
	        //dindex = DINDEX(dv,ip);
	        //GINDEX(m_off,m_pos,in+dict,dindex,in);
	        //UPDATE_I(dict,0,dindex,ip,in);
			dindex = ((dv * LZO_HASH_VALUE) >> 19) & 0x1FFF;	/* Determine dictionary index that maps to the new data value.		*/
			m_pos = in + dict[dindex];	/* Obtain absolute address of the current dictionary entry match.	*/
			dict[dindex] = ip-in;		/* Update dictionary entry to point to the latest value, store relative offset. */

	        if (dv != UA_GET32(m_pos))
	            goto literal;
        }


		/* a match */
        ii -= ti; ti = 0;
        {
	        register lzo_uint t = (ip-ii);
	        if (t != 0)
	        {
		        if (t <= 3)
	            {
		            op[-2] |= LZO_BYTE(t);
	               UA_COPY32(op, ii);
	                op += t;
				}
				else if (t <= 16)
				{
					*op++ = LZO_BYTE(t - 3);
					UA_COPY32(op, ii);
					UA_COPY32(op+4, ii+4);
					UA_COPY32(op+8, ii+8);
					UA_COPY32(op+12, ii+12);
					op += t;
				}
				else
				{
					if (t <= 18)
						*op++ = LZO_BYTE(t - 3);
					else
					{
						register lzo_uint tt = t - 18;
						*op++ = 0;
						while (tt > 255)
						{
							tt -= 255;
							* (volatile unsigned char *) op++ = 0;
						}
						*op++ = LZO_BYTE(tt);
					}
					/* Uncompressed Litteral Copy using Agner Fog's Memcpy Routine */
#if 1
					A_memcpy(op,ii,t);
					op += t;
					ii += t;
#endif

#if 0
					do {
						UA_COPY32(op, ii);
						UA_COPY32(op+4, ii+4);
						UA_COPY32(op+8, ii+8);
						UA_COPY32(op+12, ii+12);
						op += 16; ii += 16; t -= 16;
					} while (t >= 16); if (t > 0)

					{ do *op++ = *ii++; while (--t > 0); }
#endif
				}
			}
        }
        


		m_len = 4;
        {
//			unsigned int bytematch; //Removed for cache aligned reads
	        lzo_uint32 v;
		    v = UA_GET32(ip + m_len) ^ UA_GET32(m_pos + m_len);
	        if (v == 0) 
			{
				do {
					m_len += 4;
					v = UA_GET32(ip + m_len) ^ UA_GET32(m_pos + m_len);
					if (ip + m_len >= ip_end)
						goto m_len_done;
				} while (v == 0);
	        }

	        //m_len += lzo_bitops_ctz32(v) / CHAR_BIT;
//			_BitScanForward((unsigned long*)&bytematch,v);	/* ASM BSF */ //Removed for cache aligned reads
//			m_len += (bytematch/8);	//Removed for cache aligned reads
		}


m_len_done:
        m_off = (ip-m_pos);		
        ip += m_len;
        ii = ip;
        if (m_len <= M2_MAX_LEN && m_off <= M2_MAX_OFFSET)
        {
            m_off -= 1;
            *op++ = LZO_BYTE(((m_len - 1) << 5) | ((m_off & 7) << 2));
            *op++ = LZO_BYTE(m_off >> 3);
        }
        else if (m_off <= M3_MAX_OFFSET)
        {
            m_off -= 1;
            if (m_len <= M3_MAX_LEN)
                *op++ = LZO_BYTE(M3_MARKER | (m_len - 2));
            else
            {
                m_len -= M3_MAX_LEN;
                *op++ = M3_MARKER | 0;
                while (m_len > 255)
                {
                    m_len -= 255;
                    * (volatile unsigned char *) op++ = 0;
                }
                *op++ = LZO_BYTE(m_len);
            }
            *op++ = LZO_BYTE(m_off << 2);
            *op++ = LZO_BYTE(m_off >> 6);
        }
        else
        {
            m_off -= 0x4000;
            if (m_len <= M4_MAX_LEN)
                *op++ = LZO_BYTE(M4_MARKER | ((m_off >> 11) & 8) | (m_len - 2));
            else
            {
                m_len -= M4_MAX_LEN;
                *op++ = LZO_BYTE(M4_MARKER | ((m_off >> 11) & 8));
                while (m_len > 255)
                {
                    m_len -= 255;
                    * (volatile unsigned char *) op++ = 0;
                }
                *op++ = LZO_BYTE(m_len);
            }
            *op++ = LZO_BYTE(m_off << 2);
            *op++ = LZO_BYTE(m_off >> 6);
        }
        goto next;
    }

    *out_len = op - out;
    return in_end - (ii-ti);
}
