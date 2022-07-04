#ifndef FAST_MEMCPY_LZO
#define FAST_MEMCPY_LZO

int lzo1x_1_15_FASTMEMCPYcompress( const unsigned char* in, unsigned int  in_len,
                         unsigned char* out, unsigned int* out_len,
                         void* wrkmem );




#endif