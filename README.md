# lzo-compression-benchmarker
Benchmark Software created for 2012 IEEE 24th International Symposium on Computer Architecture and High Performance Computing Conference Paper "Compression Speed Enhancements to LZO for Multi-core Systems"  
https://doi.org/10.1109/SBAC-PAD.2012.29  



Usage:  compressionTest.exe filename compressionType numIterations blockSize  

Where:  
    filename is the full path to the file to be compressed  
    compressionType is an integer in the range 0 to 5, with  
        0 = Baseline LZO 1x-1-15 compression  
        1 = LZO with SSE Optimized Memory Copy  
        2 = LZO with 32-bit Traversal & Unaligned Cache Reads  
        3 = LZO with Cache Aligned Read Optimization  
        4 = Multicore LZO compression  
        5 = LZO with Multicore/SSE Memory Copy/Cache Aligned Reads  
     numIterations is the number of times to perform compression on the dataset  
     blockSize is an optional input parameter.  If specified, it defines the size of the          blocks of data to be compressed.  Otherwise the block size defaults to 256kb.  

The output files containing the compression analysis data will be located in the folder:  
    C:\Compression_Test\  
This folder must be created prior to running the program.  

