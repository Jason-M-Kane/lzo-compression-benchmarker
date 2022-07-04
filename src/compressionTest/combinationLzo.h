#ifndef COMBO_LZO_H
#define COMBO_LZO_H

#include <windows.h>
#include "multiCoreLzo.h"



/* Function Prototypes */
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
	reassembleType** reassembleInst);

int lzo1x_1_15_Combo_control(const unsigned char* in, unsigned int  in_len,
								 unsigned char* out, unsigned int* out_len,
								 int numLzoThreads, int blockSize, 
								 tmpOutBufStruct* tmpBufferArray,
								 unsigned int numTmpBuffers, 
								 HANDLE* hSemTaskCmplete,
								 threadCommsStruct* threadComms,
								 reassembleType* reassembleInfo);


#endif
