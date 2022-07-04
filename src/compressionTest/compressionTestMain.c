#pragma warning(disable:4996)

#include <stdio.h>
#include <windows.h>
#include "origLzo.h"
#include "MatchLzo.h"
#include "fasterMemcpy.h"
#include "origLzoNotAligned32.h"
#include "origLzoAligned32.h"
#include "multiCoreLzo.h"
#include "combinationLzo.h"

#define DEFAULT_BLOCK_SIZE		262144	/* 256kB */
#define OUTPUT_FILE_NAME        "C:\\Compression_Test\\CompressionTestOutput.txt"

/* Compression Modes */
#define OLD_LZO					0   /* Baseline LZO 1x-1-15 Test */
#define FAST_MEMCPY				1   /* Faster Memory Copying*/
#define NOT_ALIGNED_32			2   /* Unaligned Read Test, Skip 4 bytes */
#define ALIGNED_32				3	/* Aligned Read Test */
#define MULTICORE				4   /* Multicore Test */
#define COMBO					5   /* 3x Combined Test */

/* Size of Memory Buffer */
#define BUFFER_SIZE	(256*1024*1024)		/* NOTE: MUST BE A MULTIPLE OF BLOCK SIZE IN ORDER FOR PROGRAM TO WORK PROPERLY!!! */
											/* Can get around this with more coding, but for benchmarking purposes, not necessary or useful */
#define BUFFER_SIZE_SMALL (32*1024*1024)
#define NUM_READS (BUFFER_SIZE/BUFFER_SIZE_SMALL)


/* Function to fill a buffer of size BUFFER_SIZE bytes in increments of BUFFER_SIZE_SMALL */
int fillBuffer(HANDLE infile, unsigned char* inputBuffer, DWORD* numRead)
{
	int x;
	unsigned int totalRead = 0;
	unsigned char* pInput = inputBuffer;

	for(x=0;x<NUM_READS;x++)
	{
		/* Fill Buffer For the First Time */
		if( ReadFile(infile,pInput,BUFFER_SIZE_SMALL,numRead,NULL) == 0)
		{
			printf("ReadFile failure %u.\n",GetLastError());
			_aligned_free(inputBuffer);
			CloseHandle(infile);
			return -1;
		}
		totalRead+=*numRead;
		pInput+=*numRead;

		if(*numRead < BUFFER_SIZE_SMALL)
		{
			/* EOF */
			return 0;
		}
	}

	return 0;
}




int main(int argc, char** argv)
{
	int aa,numIterations;
	HANDLE infile = NULL;
	DWORD numRead;
	FILE* outfile = NULL;
	
	unsigned int numLZOThreads;	//for multicore version
	threadCommsStruct* instThreadComms; 
	reassembleType* reassembleInst;
	unsigned char* instDictMem;
	HANDLE* instNodeHandle;
	HANDLE* instTaskCmpleteHandle;
	tmpOutBufStruct* instTmpBuffer;
	unsigned int compressionBlockSizeBytes;
	unsigned int numTmpOutputBuffers;
	unsigned int tmpOutputBufferSizeBytes;
	unsigned int bufferedBytes = 0;

	unsigned int outLength;
	unsigned int blockSize;
	static char inputfname[300];
	unsigned char* inputBuffer = NULL;
	unsigned char* pInputBuffer = NULL;
	char* dictMem = NULL;
	unsigned char* outputBuffer = NULL;
	float time = 0.0;
	int compressionType;
	LARGE_INTEGER startCount,endCount,fullCount,freq,fileSize,amountCompressed,dist;
	QueryPerformanceFrequency( &freq );
	dist.QuadPart = 0;

	compressionType = OLD_LZO;

	/***********************************************/
	/* Parse command line parameters if they exist */
	/***********************************************/
	if( (argc >= 4) && (strlen(argv[1]) < 300) )
	{
		strcpy(inputfname,argv[1]);
		compressionType = atoi(argv[2]);
		numIterations =  atoi(argv[3]);
		compressionBlockSizeBytes = DEFAULT_BLOCK_SIZE;

		/* Check for Input Block Size Option, If it doesnt exist default 256kB is used */
		if(argc >= 5)
		{
			compressionBlockSizeBytes = atoi(argv[4]);
			compressionBlockSizeBytes *= 1024;
			if( (compressionBlockSizeBytes < 1024) || ((compressionBlockSizeBytes % 1024) != 0) )
			{
				printf("Error:  Compression Block Size must be a multiple of 1024\n");
				return -1;
			}
		}

		/* MultiCore and All Enhancements */
		if((compressionType == 4) || (compressionType == 5))
		{
			if(argc != 7)
			{
				printf("Error in arguments: [Fname] [CompressAlg] [NumIter] [InputBlockSizeKB (Opt)] [NumThreads (MC Only)] [NumOutputBuffers (MC Only)]\n");
				return -1;
			}
			numLZOThreads = atoi(argv[5]);
			numTmpOutputBuffers = atoi(argv[6]);
			if((numLZOThreads < 0) || (numTmpOutputBuffers < numLZOThreads))
			{
				printf("Error in arguments: [Fname] [CompressAlg] [NumIter] [InputBlockSizeKB (Opt)] [NumThreads (MC Only)] [NumOutputBuffers (MC Only)]\n");
				printf("\tnumTmpOutputBuffers Must be >= numLZOThreads\n");
				return -1;
			}
			if(numLZOThreads > (MAXIMUM_WAIT_OBJECTS-1))	/* Reserve 1 Thread for File Reassembly */
			{
				printf("Error, number of threads cannot exceed %d.  Windows Limitation on WaitForMultipleObjects().\n",(MAXIMUM_WAIT_OBJECTS-1));
				return -1;
			}
		}
		else
		{
			/* Not a compression type using multiple cores, but wrong # of arguments */
			if(argc > 5)
			{
				printf("Error in arguments: [Fname] [CompressAlg] [NumIter] [InputBlockSizeKB (Opt)] [NumThreads (MC Only)] [NumOutputBuffers (MC Only)]\n");
				return -1;
			}
		}
	}
	else
	{
		/* Wrong # of arugments */
		printf("Error in arguments: [Fname] [CompressAlg] [NumIter] [InputBlockSizeKB (Opt)] [NumThreads (MC Only)] [NumOutputBuffers (MC Only)]\n");
		return -1;
	}


	/* Open the file for reading */
	infile = CreateFile((LPCSTR)inputfname,GENERIC_READ,FILE_SHARE_READ,
		NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_NO_BUFFERING,NULL);
	if(infile == INVALID_HANDLE_VALUE)
	{
		printf("Error opening Handle to input file %u.\n",GetLastError());
		return -1;
	}

	/* Determine the File Size */
	if( GetFileSizeEx(infile,&fileSize) == 0)
	{
		printf("Error GetFileSizeEx failed\n");
		CloseHandle(infile);
		return -1;
	}
	amountCompressed.QuadPart = fileSize.QuadPart; /* Will be compressing the entire file */

	/* Allocate memory buffers */
	inputBuffer = (unsigned char*)_aligned_malloc(BUFFER_SIZE,16);
	if(inputBuffer == NULL)
	{
		printf("Error allocating memory.\n");
		CloseHandle(infile);
		return -1;
	}
	outputBuffer = (unsigned char*)_aligned_malloc(BUFFER_SIZE+ (size_t)(0.1*BUFFER_SIZE),16);
	if(outputBuffer == NULL)
	{
		printf("Error allocating memory.\n");
		CloseHandle(infile);
		return -1;
	}
	dictMem = (char*)_aligned_malloc(1024*1024*1,16);	/* 1MB */
	if(dictMem == NULL)
	{
		printf("Error allocating memory.\n");
		CloseHandle(infile);
		return -1;
	}

	
	printf("Beginning Compression of %s, Size = %I64d bytes, Iter=%d.\n",inputfname,fileSize.QuadPart,numIterations);

	if((compressionType == MULTICORE) || (compressionType == COMBO))
	{
		tmpOutputBufferSizeBytes = compressionBlockSizeBytes + (unsigned int)(0.10 * (float)compressionBlockSizeBytes);

		instThreadComms = NULL;
		instDictMem = NULL;
		instNodeHandle = NULL;
		instTaskCmpleteHandle = NULL;
		instTmpBuffer = NULL;


		if(compressionType == MULTICORE)
		{
			if( initMultiCoreLZO(INITIALIZE_LZO, numLZOThreads, 
				&instThreadComms, &instDictMem, &instNodeHandle, &instTaskCmpleteHandle,
				&instTmpBuffer,numTmpOutputBuffers, tmpOutputBufferSizeBytes,&reassembleInst) != 0 )
			{
				printf("Multicore LZO Initialization Failed.\n");
				return -1;
			}
		}
		else
		{
			if( initComboLZO(INITIALIZE_LZO, numLZOThreads, 
				&instThreadComms, &instDictMem, &instNodeHandle, &instTaskCmpleteHandle,
				&instTmpBuffer,numTmpOutputBuffers, tmpOutputBufferSizeBytes,&reassembleInst) != 0 )
			{
				printf("COMBO LZO Initialization Failed.\n");
				return -1;
			}
		}
	}


	for(aa=0;aa<numIterations;aa++)
	{
		fullCount.QuadPart = 0;
		pInputBuffer = inputBuffer;
		fileSize.QuadPart = amountCompressed.QuadPart;

		/* Rewind File to Beginning */
		SetFilePointerEx(infile,dist,NULL,FILE_BEGIN);

		/* Fill Buffer For the First Time */
		if( fillBuffer(infile, inputBuffer, &numRead) < 0 )
		{
			_aligned_free(inputBuffer);
			_aligned_free(outputBuffer);
			CloseHandle(infile);
			return -1;
		}
		bufferedBytes = numRead;


		/**********************************/
		/* Original LZO 1x-1-15 Algorithm */
		/**********************************/
		if(compressionType == OLD_LZO)
		{
			unsigned int totalOutlen = 0;
			
			/* Set the input block size */
			blockSize = compressionBlockSizeBytes;
			if(fileSize.QuadPart < (LONGLONG) blockSize)
				blockSize = (unsigned int)fileSize.QuadPart;

			/* Get Start Count and Begin Iterating until FileSize <= 0 */
			QueryPerformanceCounter(&startCount);
			while(fileSize.QuadPart > 0)
			{
				lzo1x_1_15_compress (pInputBuffer, blockSize, 
									outputBuffer, &outLength, dictMem);
				totalOutlen+=outLength;						//Update Length of Output Data
				pInputBuffer+=blockSize;					//Advance Input Pointer

				fileSize.QuadPart -= blockSize;				//Update Remaining Filesize
				bufferedBytes -= blockSize;					//Update Remaining Bytes in the current memory read buffer
				if(fileSize.QuadPart < blockSize)			//Update Block Size when only a remainder size exists
					blockSize = (unsigned int)fileSize.QuadPart;

				//Determine when to read more input data from the input file
				//Do not count time spent reading from the input file against the throughput
				if((fileSize.QuadPart > 0) && (bufferedBytes <= 0))
				{
					//Stop Timer and update number of ticks currently expired
					QueryPerformanceCounter(&endCount);
					if(endCount.QuadPart > startCount.QuadPart)
					{
						fullCount.QuadPart += endCount.QuadPart - startCount.QuadPart;
					}
					else
					{
						//Account for roll-over situation if it occurs
						fullCount.QuadPart += (0xFFFFFFFFFFFFFFFF - endCount.QuadPart) + startCount.QuadPart + 1;
					}

					if(bufferedBytes != 0)
						printf("Warning, Buffered Bytes != 0, unexpected behavior.\n");

					if( fillBuffer(infile, inputBuffer, &numRead) < 0 )
					{
						_aligned_free(inputBuffer);
						_aligned_free(outputBuffer);
						CloseHandle(infile);
						return -1;
					}
					pInputBuffer = inputBuffer;
					bufferedBytes = numRead;

					//Restart Timer
					QueryPerformanceCounter(&startCount);
				}
			}
			outLength = totalOutlen;
		}


		/*************************************************************************************/
		/* Memcpy Optimized version of LZO 1x-1-15 using Agner Fog's Vecotorized SSE Routine */
		/*************************************************************************************/
		else if(compressionType == FAST_MEMCPY)
		{
			unsigned int totalOutlen = 0;
			
			/* Set the input block size */
			blockSize = compressionBlockSizeBytes;
			if(fileSize.QuadPart < (LONGLONG) blockSize)
				blockSize = (unsigned int)fileSize.QuadPart;

			/* Get Start Count and Begin Iterating until FileSize <= 0 */
			QueryPerformanceCounter(&startCount);
			while(fileSize.QuadPart > 0)
			{
				lzo1x_1_15_FASTMEMCPYcompress (pInputBuffer, blockSize, 
									outputBuffer, &outLength, dictMem);
				totalOutlen+=outLength;						//Update Length of Output Data
				pInputBuffer+=blockSize;					//Advance Input Pointer

				fileSize.QuadPart -= blockSize;				//Update Remaining Filesize
				bufferedBytes -= blockSize;					//Update Remaining Bytes in the current memory read buffer
				if(fileSize.QuadPart < blockSize)			//Update Block Size when only a remainder size exists
					blockSize = (unsigned int)fileSize.QuadPart;

				//Determine when to read more input data from the input file
				//Do not count time spent reading from the input file against the throughput
				if((fileSize.QuadPart > 0) && (bufferedBytes <= 0))
				{
					//Stop Timer and update number of ticks currently expired
					QueryPerformanceCounter(&endCount);
					if(endCount.QuadPart > startCount.QuadPart)
					{
						fullCount.QuadPart += endCount.QuadPart - startCount.QuadPart;
					}
					else
					{
						//Account for roll-over situation if it occurs
						fullCount.QuadPart += (0xFFFFFFFFFFFFFFFF - endCount.QuadPart) + startCount.QuadPart + 1;
					}

					if(bufferedBytes != 0)
						printf("Warning, Buffered Bytes != 0, unexpected behavior.\n");

					if( fillBuffer(infile, inputBuffer, &numRead) < 0 )
					{
						_aligned_free(inputBuffer);
						_aligned_free(outputBuffer);
						CloseHandle(infile);
						return -1;
					}
					pInputBuffer = inputBuffer;
					bufferedBytes = numRead;

					//Restart Timer
					QueryPerformanceCounter(&startCount);
				}
			}
			outLength = totalOutlen;
		}


		/*********************************************************/
		/* Cache Aligned Access Optimized Version of LZO 1x-1-15 */
		/*********************************************************/
		else if(compressionType == ALIGNED_32)
		{
			unsigned int totalOutlen = 0;
			
			/* Set the input block size */
			blockSize = compressionBlockSizeBytes;
			if(fileSize.QuadPart < (LONGLONG) blockSize)
				blockSize = (unsigned int)fileSize.QuadPart;

			/* Get Start Count and Begin Iterating until FileSize <= 0 */
			QueryPerformanceCounter(&startCount);
			while(fileSize.QuadPart > 0)
			{
				lzo1x_1_15_compress_32_ALIGNED (pInputBuffer, blockSize, 
									outputBuffer, &outLength, dictMem);
				totalOutlen+=outLength;						//Update Length of Output Data
				pInputBuffer+=blockSize;					//Advance Input Pointer

				fileSize.QuadPart -= blockSize;				//Update Remaining Filesize
				bufferedBytes -= blockSize;					//Update Remaining Bytes in the current memory read buffer
				if(fileSize.QuadPart < blockSize)			//Update Block Size when only a remainder size exists
					blockSize = (unsigned int)fileSize.QuadPart;

				//Determine when to read more input data from the input file
				//Do not count time spent reading from the input file against the throughput
				if((fileSize.QuadPart > 0) && (bufferedBytes <= 0))
				{
					//Stop Timer and update number of ticks currently expired
					QueryPerformanceCounter(&endCount);
					if(endCount.QuadPart > startCount.QuadPart)
					{
						fullCount.QuadPart += endCount.QuadPart - startCount.QuadPart;
					}
					else
					{
						//Account for roll-over situation if it occurs
						fullCount.QuadPart += (0xFFFFFFFFFFFFFFFF - endCount.QuadPart) + startCount.QuadPart + 1;
					}

					if(bufferedBytes != 0)
						printf("Warning, Buffered Bytes != 0, unexpected behavior.\n");

					if( fillBuffer(infile, inputBuffer, &numRead) < 0 )
					{
						_aligned_free(inputBuffer);
						_aligned_free(outputBuffer);
						CloseHandle(infile);
						return -1;
					}
					pInputBuffer = inputBuffer;
					bufferedBytes = numRead;

					//Restart Timer
					QueryPerformanceCounter(&startCount);
				}
			}
			outLength = totalOutlen;
		}


		/*******************************************************************************************/
		/* 32-bit Word Traversal Version of LZO 1x-1-15, for comparison with Cache Aligned Version */
		/*******************************************************************************************/
		else if(compressionType == NOT_ALIGNED_32)
		{
			unsigned int totalOutlen = 0;
			
			/* Set the input block size */
			blockSize = compressionBlockSizeBytes;
			if(fileSize.QuadPart < (LONGLONG) blockSize)
				blockSize = (unsigned int)fileSize.QuadPart;

			/* Get Start Count and Begin Iterating until FileSize <= 0 */
			QueryPerformanceCounter(&startCount);
			while(fileSize.QuadPart > 0)
			{
				lzo1x_1_15_compress_32_NOT_ALIGNED (pInputBuffer, blockSize, 
									outputBuffer, &outLength, dictMem);
				totalOutlen+=outLength;						//Update Length of Output Data
				pInputBuffer+=blockSize;					//Advance Input Pointer

				fileSize.QuadPart -= blockSize;				//Update Remaining Filesize
				bufferedBytes -= blockSize;					//Update Remaining Bytes in the current memory read buffer
				if(fileSize.QuadPart < blockSize)			//Update Block Size when only a remainder size exists
					blockSize = (unsigned int)fileSize.QuadPart;

				//Determine when to read more input data from the input file
				//Do not count time spent reading from the input file against the throughput
				if((fileSize.QuadPart > 0) && (bufferedBytes <= 0))
				{
					//Stop Timer and update number of ticks currently expired
					QueryPerformanceCounter(&endCount);
					if(endCount.QuadPart > startCount.QuadPart)
					{
						fullCount.QuadPart += endCount.QuadPart - startCount.QuadPart;
					}
					else
					{
						//Account for roll-over situation if it occurs
						fullCount.QuadPart += (0xFFFFFFFFFFFFFFFF - endCount.QuadPart) + startCount.QuadPart + 1;
					}

					if(bufferedBytes != 0)
						printf("Warning, Buffered Bytes != 0, unexpected behavior.\n");

					if( fillBuffer(infile, inputBuffer, &numRead) < 0 )
					{
						_aligned_free(inputBuffer);
						_aligned_free(outputBuffer);
						CloseHandle(infile);
						return -1;
					}
					pInputBuffer = inputBuffer;
					bufferedBytes = numRead;

					//Restart Timer
					QueryPerformanceCounter(&startCount);
				}
			}
			outLength = totalOutlen;
		}


		/**************************************************/
		/* Multiple Core Optimized Version of LZO 1x-1-15 */
		/**************************************************/
		else if(compressionType == MULTICORE)
		{
			unsigned int totalOutlen = 0;
			
			/* Set the input block size */
			blockSize = compressionBlockSizeBytes;
			if(fileSize.QuadPart < (LONGLONG) blockSize)
				blockSize = (unsigned int)fileSize.QuadPart;

			/* Multicore LZO Algorithm */
			QueryPerformanceCounter(&startCount);
			outLength = 0;
			while(fileSize.QuadPart > 0)
			{
				if( lzo1x_1_15_multiCore_control(
					inputBuffer, 
					(unsigned int)  numRead,
					outputBuffer, 
					&outLength,
					numLZOThreads, 
					blockSize, 
					instTmpBuffer,
					numTmpOutputBuffers,
					instTaskCmpleteHandle,
					instThreadComms,reassembleInst) != 0)
				{
					printf("Multicore LZO Compression Failed.\n");
				}

				totalOutlen+=outLength;						//Update Length of Output Data
				fileSize.QuadPart -= numRead;				//Update Remaining Filesize

				//Determine when to read more input data from the input file
				//Do not count time spent reading from the input file against the throughput
				if(fileSize.QuadPart > 0)
				{
					//Stop Timer and update number of ticks currently expired
					QueryPerformanceCounter(&endCount);
					if(endCount.QuadPart > startCount.QuadPart)
					{
						fullCount.QuadPart += endCount.QuadPart - startCount.QuadPart;
					}
					else
					{
						//Account for roll-over situation if it occurs
						fullCount.QuadPart += (0xFFFFFFFFFFFFFFFF - endCount.QuadPart) + startCount.QuadPart + 1;
					}
					if( fillBuffer(infile, inputBuffer, &numRead) < 0 )
					{
						_aligned_free(inputBuffer);
						_aligned_free(outputBuffer);
						CloseHandle(infile);
						return -1;
					}

					//Restart Timer
					QueryPerformanceCounter(&startCount);
				}
			}
			outLength = totalOutlen;
		}


		/*************************************************************************************************************/
		/* Optimized LZO 1x-1-15, Consisting of 3 enhancements combined:  SSE Memcpy, Cache Aligned Reads, MultiCore */
		/*************************************************************************************************************/
		else if(compressionType == COMBO)
		{
			unsigned int totalOutlen = 0;
			
			/* Set the input block size */
			blockSize = compressionBlockSizeBytes;
			if(fileSize.QuadPart < (LONGLONG) blockSize)
				blockSize = (unsigned int)fileSize.QuadPart;

			/* Combo LZO Algorithm */
			QueryPerformanceCounter(&startCount);
			outLength = 0;
			while(fileSize.QuadPart > 0)
			{
				if( lzo1x_1_15_Combo_control(
					inputBuffer, 
					(unsigned int)  numRead,
					outputBuffer, 
					&outLength,
					numLZOThreads, 
					blockSize, 
					instTmpBuffer,
					numTmpOutputBuffers,
					instTaskCmpleteHandle,
					instThreadComms,reassembleInst) != 0)
				{
					printf("Combo LZO Compression Failed.\n");
				}
			
				totalOutlen+=outLength;						//Update Length of Output Data
				fileSize.QuadPart -= numRead;				//Update Remaining Filesize

				//Determine when to read more input data from the input file
				//Do not count time spent reading from the input file against the throughput
				if(fileSize.QuadPart > 0)
				{
					//Stop Timer and update number of ticks currently expired
					QueryPerformanceCounter(&endCount);
					if(endCount.QuadPart > startCount.QuadPart)
					{
						fullCount.QuadPart += endCount.QuadPart - startCount.QuadPart;
					}
					else
					{
						//Account for roll-over situation if it occurs
						fullCount.QuadPart += (0xFFFFFFFFFFFFFFFF - endCount.QuadPart) + startCount.QuadPart + 1;
					}
					if( fillBuffer(infile, inputBuffer, &numRead) < 0 )
					{
						_aligned_free(inputBuffer);
						_aligned_free(outputBuffer);
						CloseHandle(infile);
						return -1;
					}

					//Restart Timer
					QueryPerformanceCounter(&startCount);
				}
			}
			outLength = totalOutlen;
		}


		else
		{
			printf("Unknown Method.\n");
			CloseHandle(infile);
			_aligned_free(inputBuffer);
			return -1;
		}
		
		/* Determine Time Spent for Compression */
		QueryPerformanceCounter(&endCount);
		fullCount.QuadPart = endCount.QuadPart-startCount.QuadPart+fullCount.QuadPart;
		time += (float)((double)(fullCount.QuadPart) / (double)(freq.QuadPart));
	}

	/* MultiCore Cleanup */
	if(compressionType == MULTICORE)
	{
		if( initMultiCoreLZO(CLEANUP_LZO, numLZOThreads, 
				&instThreadComms, &instDictMem, &instNodeHandle, &instTaskCmpleteHandle,
				&instTmpBuffer,numTmpOutputBuffers, tmpOutputBufferSizeBytes,&reassembleInst) != 0 )
			{
				printf("Multicore LZO Cleanup Failed.\n");
				return -1;
			}
	}

	/* Combo Cleanup */
	if(compressionType == COMBO)
	{
		if( initComboLZO(CLEANUP_LZO, numLZOThreads, 
				&instThreadComms, &instDictMem, &instNodeHandle, &instTaskCmpleteHandle,
				&instTmpBuffer,numTmpOutputBuffers, tmpOutputBufferSizeBytes,&reassembleInst) != 0 )
			{
				printf("COMBO LZO Cleanup Failed.\n");
				return -1;
			}
	}


	/* Print Information to CRT */
	printf("\n\nCompression took %f seconds.\n",time);
	printf("Output Size is %d bytes (%f%% compression; %f bits/byte).\n",outLength,((float)outLength/(float)amountCompressed.QuadPart)*100.0, 
		((float)outLength/(float)amountCompressed.QuadPart)*8.0);
	printf("Throughput: %f MBytes/sec\n\n", (((float)amountCompressed.QuadPart*(float)aa)/time) / (float)(1024.0*1024.0) );
	fflush(stdin);
//	getchar();

	/* Print Information out to a File (Append so Batch Jobs are fine) */
	outfile = fopen(OUTPUT_FILE_NAME,"a");
	if(outfile == NULL)
	{
		printf("Error opening output file for writing.\n");
	}
	else
	{
		int x;

		fprintf(outfile, "\n\n============== BEGIN TEST =================\n\n");
		fprintf(outfile,"Input Format: [Fname] [CompressAlg] [NumIter] [InputBlockSizeKB] [NumThreads (MC Only)] [NumOutputBuffers (MC Only)]\n");
		fprintf(outfile,"\tOLD_LZO=0,FAST_MEMCPY=1,NOT_ALIGNED_32=2,ALIGNED_32=3,MULTICORE=4,COMBO=5\n");
		fprintf(outfile, "\nInput Parameters: \n");
		for(x = 0; x < argc; x++)
			fprintf(outfile,"\t%d: %s\n",x+1,argv[x]);
		fprintf(outfile,"\n\nCompression took %f seconds.\n",time);
		fprintf(outfile,"Output Size is %d bytes (%f%% compression; %f bits/byte).\n",outLength,((float)outLength/(float)amountCompressed.QuadPart)*100.0, 
			((float)outLength/(float)amountCompressed.QuadPart)*8.0);
		fprintf(outfile,"Throughput: %f MBytes/sec\n\n", (((float)amountCompressed.QuadPart*(float)aa)/time) / (float)(1024.0*1024.0) );
		fprintf(outfile, "\n============== END TEST =================\n\n");
		fclose(outfile);
	}

	/* Cleanup */
	CloseHandle(infile);
	_aligned_free(inputBuffer);
	_aligned_free(dictMem);
	_aligned_free(outputBuffer);

	return 0;
}




