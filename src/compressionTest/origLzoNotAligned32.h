#ifndef NOTALIGNED_LZO_H
#define NOTALIGNED_LZO_H


int lzo1x_1_15_compress_32_NOT_ALIGNED( const unsigned char* in, unsigned int  in_len,
                         unsigned char* out, unsigned int* out_len,
                         void* wrkmem );



#endif
