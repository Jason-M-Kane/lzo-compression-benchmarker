# lzo-compression-benchmarker
Benchmark Software created for 2012 IEEE 24th International Symposium on Computer Architecture and High Performance Computing Conference Paper "Compression Speed Enhancements to LZO for Multi-core Systems"  
https://doi.org/10.1109/SBAC-PAD.2012.29  



**Usage:  compressionTest.exe filename compressionType numIterations [blockSize] [NumThreads (MC Only)] [NumOutputBuffers (MC Only)]**  

Where:  
&nbsp;&nbsp;&nbsp;&nbsp;*filename* is the full path to the file to be compressed  
&nbsp;&nbsp;&nbsp;&nbsp;*compressionType* is an integer in the range 0 to 5, with  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;0 = Baseline LZO 1x-1-15 compression  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;1 = LZO with SSE Optimized Memory Copy  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;2 = LZO with 32-bit Traversal & Unaligned Cache Reads  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;3 = LZO with Cache Aligned Read Optimization  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;4 = Multicore LZO compression  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;5 = LZO with Multicore/SSE Memory Copy/Cache Aligned Reads  
&nbsp;&nbsp;&nbsp;&nbsp;*numIterations* is the number of times to perform compression on the dataset  
&nbsp;&nbsp;&nbsp;&nbsp;*blockSize* is an optional input parameter.  If specified, it defines the size of the          
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;blocks of data to be compressed.  Otherwise the block size defaults to 256kb.  
&nbsp;&nbsp;&nbsp;&nbsp;*NumThreads* is the number of compression threads to use (Multicore & Combo Only)  
&nbsp;&nbsp;&nbsp;&nbsp;*NumOutputBuffers* is the number of output buffers to use (Multicore & Combo Only)  

The output files containing the compression analysis data will be located in the folder:  
&nbsp;&nbsp;&nbsp;&nbsp;C:\Compression_Test\  
This folder must be created prior to running the program.  

