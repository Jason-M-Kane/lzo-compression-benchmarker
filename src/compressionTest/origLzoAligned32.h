#ifndef ALIGNED_LZO_H
#define ALIGNED_LZO_H


int lzo1x_1_15_compress_32_ALIGNED( const unsigned char* in, unsigned int  in_len,
                         unsigned char* out, unsigned int* out_len,
                         void* wrkmem );



#endif
