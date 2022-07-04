#include <stdio.h>
#include <memory.h>
#include <intrin.h>
#include <xmmintrin.h>


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


static unsigned int
do_compress ( const unsigned char* in , unsigned int  in_len,
                    unsigned char* out, unsigned int* out_len,
                    unsigned int  ti,  void* wrkmem);




int lzo1x_1_15_compress( const unsigned char* in, unsigned int  in_len,
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

        ll = LZO_MIN(ll, 49152);
        ll_end = ((unsigned int)ip + ll);
        if ((ll_end + ((t + ll) >> 5)) <= ll_end || (const unsigned char*)(ll_end + ((t + ll) >> 5)) <= ip + ll)
            break;

        memset(wrkmem, 0, ((unsigned int)1 << 13 /*DBITS*/) * sizeof(unsigned short));

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
        do *op++ = *ii++; while (--t > 0);
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

	__m128i *m1,*m2,*m3,*m4,*DBITS_MASK;
	__declspec(align(16)) unsigned int hashValue[4] = {0x1824429D,0x0,0x1824429D,0x0};
	__declspec(align(16)) unsigned int dictbitsMaskArray[4] = {0x00001FFF,0x0,0x00001FFF,0x0};
	__declspec(align(16)) unsigned int hashResultA[4] = {0,0,0,0};
	__declspec(align(16)) unsigned int hashResultB[4] = {0,0,0,0};
	DBITS_MASK = (__m128i*) dictbitsMaskArray;

	
	//Assign 128-bit integer pointers
	m1 = (__m128i*)hashValue;
	m3 = (__m128i*)hashResultA;
	m4 = (__m128i*)hashResultB;
	DBITS_MASK = (__m128i*) dictbitsMaskArray;

    op = out;
    ip = in;
    ii = ip;

    ip += ti < 4 ? 4 - ti : 0;

    for (;;)
    {
        const unsigned char* m_pos;

        lzo_uint m_off;
        lzo_uint m_len;
        {
			__declspec(align(16)) __m128i *pDV;
			__declspec(align(16)) lzo_uint32 dv[4];
			__declspec(align(16)) unsigned int hashIndicies[4];
			lzo_uint dindex;
			pDV = (__m128i*)dv;
literal:
	        ip += 1 + ((ip - ii) >> 5);
next:
			if (ip >= ip_end)
	            break;

			/* Get the Next 4 Data Values */
			dv[0] = UA_GET32(ip);
			dv[1] = UA_GET32(ip+1);
			dv[2] = UA_GET32(ip+2);
			dv[3] = UA_GET32(ip+3);

			//PMULUDQ
			*m3 = _mm_mul_epu32(*m1,*pDV);

			//PSRLDQ - Shift m2 to the right by 1 LW
			*pDV = _mm_srli_si128 (*pDV, 4);
			*m4 = _mm_mul_epu32(*m1,*pDV);	//Multiply again to get the other 2 values

			//PAND - Perform 128-bit AND so that the Hash Value will fit in the dictionary
			*m3 = _mm_and_si128 (*m3, *DBITS_MASK);  // Has 3rd  1st
			*m4 = _mm_and_si128 (*m4, *DBITS_MASK);  // Has 4th  2nd



			dindex = ((dv * LZO_HASH_VALUE) >> 19) & 0x1FFF;	/* Determine dictionary index that maps to the new data value.		*/
			m_pos = in + dict[dindex];	/* Obtain absolute address of the current dictionary entry match.	*/
			dict[dindex] = ip-in;		/* Update dictionary entry to point to the latest value, store relative offset. */

			dindex = ((dv * LZO_HASH_VALUE) >> 19) & 0x1FFF;	/* Determine dictionary index that maps to the new data value.		*/
			m_pos = in + dict[dindex];	/* Obtain absolute address of the current dictionary entry match.	*/
			dict[dindex] = ip-in;		/* Update dictionary entry to point to the latest value, store relative offset. */

			dindex = ((dv * LZO_HASH_VALUE) >> 19) & 0x1FFF;	/* Determine dictionary index that maps to the new data value.		*/
			m_pos = in + dict[dindex];	/* Obtain absolute address of the current dictionary entry match.	*/
			dict[dindex] = ip-in;		/* Update dictionary entry to point to the latest value, store relative offset. */

			dindex = ((dv * LZO_HASH_VALUE) >> 19) & 0x1FFF;	/* Determine dictionary index that maps to the new data value.		*/
			m_pos = in + dict[dindex];	/* Obtain absolute address of the current dictionary entry match.	*/
			dict[dindex] = ip-in;		/* Update dictionary entry to point to the latest value, store relative offset. */





	        if (dv != UA_GET32(m_pos))
	            goto literal;
        }

matchFound:
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
					do {
						UA_COPY32(op, ii);
						UA_COPY32(op+4, ii+4);
						UA_COPY32(op+8, ii+8);
						UA_COPY32(op+12, ii+12);
						op += 16; ii += 16; t -= 16;
					} while (t >= 16); if (t > 0)

					{ do *op++ = *ii++; while (--t > 0); }
				}
			}
        }
        


		m_len = 4;
        {
			unsigned int bytematch;
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
			_BitScanForward((unsigned long*)&bytematch,v);	/* ASM BSF */
			m_len += (bytematch/8);
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
