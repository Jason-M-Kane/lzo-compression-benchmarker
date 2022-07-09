#ifndef MULTICORE_LZO_H
#define MULTICORE_LZO_H

#include <windows.h>

/* Data Structure used as a temporary buffer */
typedef struct
{
	unsigned char* buffer;
	unsigned int bufferMaxSizeBytes;
	unsigned int bufferUsedSizeBytes;
	unsigned int blockNum;
	unsigned int id;
}tmpOutBufStruct;




typedef struct
{
	HANDLE  semCritAccess;		/* Get mutual access to shared data elements */
	HANDLE  hSemReassemble;		/* Given by Ctrl Task whenever a new cmd is ready to be run by the reassemble thread */
	HANDLE* hSemTaskCmplete;	/* Given by Reassemble Task when the last command was completed */
	int numBlocksCopied;		/* # of data blocks copied */
	int numBlocksToCopy;		/* # of compressed data blocks to be copied. */
	int action;					/* Action to perform */
	int status;					/* Status */
	unsigned char* pOutput;		/* Pointer to the beginning of the output stream */

	/****************************************************************/
	/* Data Structures modified by both Ctrl and Reassemble Threads */
	/****************************************************************/
	tmpOutBufStruct** savedTbufArray; /* Modified when tmp buffers are available to be inserted into the output stream */
	tmpOutBufStruct** savedTbufArraySpare; /* Modified when tmp buffers are available to be inserted into the output stream */
	int numSavedTbuf;			/* Number of saved off tmp buffers */
	int *freedTbufArray;		/* Modified when saved tmp buffers are freed*/
	int *tmpfreedTbufArray;		/* Temp version for reassembly thread, Modified when saved tmp buffers are freed*/
	int numFreedTbuf;			/* # of temporary buffers to be freed */
}reassembleType;


/* Data structure used to communicate compression information to threads */
typedef struct
{
	HANDLE hSemCtrl;
	HANDLE* hSemTaskCmplete;
	int threadId;
	int status;
	const unsigned char* in;
	unsigned int in_len;
	tmpOutBufStruct* tmpOutBuffer;
	unsigned short* ptrDictionary;
	LARGE_INTEGER blockStartCount, blockEndCount, blockFullCount;
}threadCommsStruct;




/* Init Actions */
#define INITIALIZE_LZO	0
#define CLEANUP_LZO		1

/* Function Prototypes */
int initMultiCoreLZO(
	int action, 
	unsigned int numThreads,
	threadCommsStruct** instThreadComms, 
	unsigned char** instDictMem,
	HANDLE** instNodeHandle,
	HANDLE** instTaskCmpleteHandle,
	tmpOutBufStruct** instTmpBuffer,
	unsigned int numTmpOutputBuffers, 
	unsigned int tmpOutputBufferSizeBytes,
	reassembleType** reassembleInst);

int lzo1x_1_15_multiCore_control(const unsigned char* in, unsigned int  in_len,
								 unsigned char* out, unsigned int* out_len,
								 int numLzoThreads, int blockSize, 
								 tmpOutBufStruct* tmpBufferArray,
								 unsigned int numTmpBuffers, 
								 HANDLE* hSemTaskCmplete,
								 threadCommsStruct* threadComms,
								 reassembleType* reassembleInfo,
								 unsigned int* numInputBlocksProcessed,
								 LARGE_INTEGER* blockFullCount);


#endif
