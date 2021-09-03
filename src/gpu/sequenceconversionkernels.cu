#include <gpu/kernels.hpp>
#include <gpu/cudaerrorcheck.cuh>
#include <hpc_helpers.cuh>
#include <config.hpp>
#include <sequencehelpers.hpp>

#include <cassert>
#include <cooperative_groups.h>


//#define DO_CHECK_CONVERSIONS

namespace cg = cooperative_groups;

namespace care{
namespace gpu{

template<class First2Bit, class First2BitHilo, class Trafo2Bit, class Trafo2BitHilo>
__global__
void checkSequenceConversionKernel(const unsigned int* const __restrict__ normalData,
        size_t normalpitchInInts, // max num ints per input sequence
        const unsigned int*  const __restrict__ hiloData,
        size_t hilopitchInInts, // max num ints per output sequence
        const int* const __restrict__ sequenceLengths,
        int numSequences,
        First2Bit first2Bit,
        First2BitHilo first2BitHilo,
        Trafo2Bit trafo2Bit,
        Trafo2BitHilo trafo2BitHilo){

    auto to_nuc = [](std::uint8_t enc){
        return SequenceHelpers::decodeBase(enc);
    };

    //use one block per sequence
    for(int index = blockIdx.x; index < numSequences; index += gridDim.x){
        const int sequenceLength = sequenceLengths[index];
        const unsigned int* const normalSeq = normalData + first2Bit(index);
        const unsigned int* const hiloSeq = hiloData + first2BitHilo(index);    
        
        for(int p = threadIdx.x; p < sequenceLength; p += blockDim.x){
            std::uint8_t encnormal = SequenceHelpers::getEncodedNuc2Bit(normalSeq, sequenceLength, p, trafo2Bit);
            char basenormal = to_nuc(encnormal);
            std::uint8_t enchilo = SequenceHelpers::getEncodedNuc2BitHiLo(hiloSeq, sequenceLength, p, trafo2BitHilo);
            char basehilo = to_nuc(enchilo);
            if(basenormal != basehilo){
                printf("error seq %d position %d, normal %c hilo %c\n", index, p, basenormal, basehilo);
            }
            assert(basenormal == basehilo);
        }
    }
}   

void callCheckSequenceConversionKernelNN(const unsigned int* normalData,
        size_t normalpitchInInts,
        const unsigned int* hiloData,
        size_t hilopitchInInts,
        const int* sequenceLengths,
        int numSequences,
        cudaStream_t stream){

    auto first2Bit = [=] __device__ (auto i){return i * normalpitchInInts;};
    auto first2BitHilo = [=] __device__ (auto i){return i * hilopitchInInts;};
    auto trafo2Bit = [=] __device__ (auto i){return i;};
    auto trafo2BitHilo = [=] __device__ (auto i){return i;};

    const int blocksize = 128;
    const int gridsize = std::min(numSequences, 65535);

    checkSequenceConversionKernel<<<gridsize,blocksize, 0, stream>>>(
        normalData,
        normalpitchInInts,
        hiloData,
        hilopitchInInts,
        sequenceLengths,
        numSequences,
        first2Bit,
        first2BitHilo,
        trafo2Bit,
        trafo2BitHilo
    ); CUDACHECKASYNC;
}

void callCheckSequenceConversionKernelNT(const unsigned int* normalData,
        size_t normalpitchInInts,
        const unsigned int* hiloData,
        size_t hilopitchInInts,
        const int* sequenceLengths,
        int numSequences,
        cudaStream_t stream){

    auto first2Bit = [=] __device__ (auto i){return i * normalpitchInInts;};
    auto first2BitHilo = [=] __device__ (auto i){return i;};
    auto trafo2Bit = [=] __device__ (auto i){return i;};
    auto trafo2BitHilo = [=] __device__ (auto i){return i * numSequences;};

    const int blocksize = 128;
    const int gridsize = std::min(numSequences, 65535);

    checkSequenceConversionKernel<<<gridsize,blocksize, 0, stream>>>(
        normalData,
        normalpitchInInts,
        hiloData,
        hilopitchInInts,
        sequenceLengths,
        numSequences,
        first2Bit,
        first2BitHilo,
        trafo2Bit,
        trafo2BitHilo
    ); CUDACHECKASYNC;
}

void callCheckSequenceConversionKernelTT(const unsigned int* normalData,
        size_t normalpitchInInts,
        const unsigned int* hiloData,
        size_t hilopitchInInts,
        const int* sequenceLengths,
        int numSequences,
        cudaStream_t stream){

    auto first2Bit = [=] __device__ (auto i){return i;};
    auto first2BitHilo = [=] __device__ (auto i){return i;};
    auto trafo2Bit = [=] __device__ (auto i){return i * numSequences;};
    auto trafo2BitHilo = [=] __device__ (auto i){return i * numSequences;};

    const int blocksize = 128;
    const int gridsize = std::min(numSequences, 65535);

    checkSequenceConversionKernel<<<gridsize, blocksize, 0, stream>>>(
        normalData,
        normalpitchInInts,
        hiloData,
        hilopitchInInts,
        sequenceLengths,
        numSequences,
        first2Bit,
        first2BitHilo,
        trafo2Bit,
        trafo2BitHilo
    ); CUDACHECKASYNC;
}

 
template<int groupsize>
__global__
void convert2BitTo2BitHiloKernelNN(
        const unsigned int* const __restrict__ inputdata,
        size_t inputpitchInInts, // max num ints per input sequence
        unsigned int*  const __restrict__ outputdata,
        size_t outputpitchInInts, // max num ints per output sequence
        const int* const __restrict__ sequenceLengths,
        const int* __restrict__ numSequencesPtr){

    const int numSequences = *numSequencesPtr;

    auto inputStartIndex = [&](auto i){return i * inputpitchInInts;};
    auto outputStartIndex = [&](auto i){return i * outputpitchInInts;};
    auto inputTrafo = [&](auto i){return i;};
    auto outputTrafo = [&](auto i){return i;};

    auto convert = [&](auto group,
                        unsigned int* out,
                        const unsigned int* in,
                        int length,
                        auto inindextrafo,
                        auto outindextrafo){

        const int inInts = SequenceHelpers::getEncodedNumInts2Bit(length);
        const int outInts = SequenceHelpers::getEncodedNumInts2BitHiLo(length);

        unsigned int* const outHi = out;
        unsigned int* const outLo = out + outindextrafo(outInts/2);

        for(int i = group.thread_rank(); i < outInts / 2; i += group.size()){
            const int outIndex = outindextrafo(i);
            const int inindex1 = inindextrafo(i*2);

            const unsigned int data1 = in[inindex1];
            const unsigned int even161 = SequenceHelpers::extractEvenBits(data1);
            const unsigned int odd161 = SequenceHelpers::extractEvenBits(data1 >> 1);

            unsigned int resultHi = odd161 << 16;
            unsigned int resultLo = even161 << 16;

            if((i < outInts / 2 - 1) || ((length-1) % 32) >= 16){
                const int inindex2 = inindextrafo(i*2 + 1);

                const unsigned int data2 = in[inindex2];
                const unsigned int even162 = SequenceHelpers::extractEvenBits(data2);
                const unsigned int odd162 = SequenceHelpers::extractEvenBits(data2 >> 1);

                resultHi = resultHi | odd162;
                resultLo = resultLo | even162;
            }

            outHi[outIndex] = resultHi;
            outLo[outIndex] = resultLo;
        }
    };

    auto group = cg::tiled_partition<groupsize>(cg::this_thread_block());
    const int numGroups = (blockDim.x * gridDim.x) / groupsize;
    const int groupId = (threadIdx.x + blockIdx.x * blockDim.x) / groupsize;

    for(int index = groupId; index < numSequences; index += numGroups){
        const int sequenceLength = sequenceLengths[index];
        const unsigned int* const in = inputdata + inputStartIndex(index);
        unsigned int* const out = outputdata + outputStartIndex(index);            

        convert(
            group,
            out,
            in,
            sequenceLength,
            inputTrafo,
            outputTrafo
        );
    } 
}

__global__
void convert2BitTo2BitHiloKernelNT(
        const unsigned int* const __restrict__ inputdata,
        size_t inputpitchInInts, // max num ints per input sequence
        unsigned int*  const __restrict__ outputdata,
        size_t outputpitchInInts, // max num ints per output sequence
        const int* const __restrict__ sequenceLengths,
        const int* __restrict__ numSequencesPtr){

    const int numSequences = *numSequencesPtr;

    auto inputStartIndex = [&](auto i){return i * inputpitchInInts;};
    auto outputStartIndex = [&](auto i){return i;};
    auto inputTrafo = [&](auto i){return i;};
    auto outputTrafo = [&](auto i){return i * numSequences;};

    auto convert = [&](auto group,
                        unsigned int* out,
                        const unsigned int* in,
                        int length,
                        auto inindextrafo,
                        auto outindextrafo){

        const int inInts = SequenceHelpers::getEncodedNumInts2Bit(length);
        const int outInts = SequenceHelpers::getEncodedNumInts2BitHiLo(length);

        unsigned int* const outHi = out;
        unsigned int* const outLo = out + outindextrafo(outInts/2);

        for(int i = group.thread_rank(); i < outInts / 2; i += group.size()){
            const int outIndex = outindextrafo(i);
            const int inindex1 = inindextrafo(i*2);

            const unsigned int data1 = in[inindex1];
            const unsigned int even161 = SequenceHelpers::extractEvenBits(data1);
            const unsigned int odd161 = SequenceHelpers::extractEvenBits(data1 >> 1);

            unsigned int resultHi = odd161 << 16;
            unsigned int resultLo = even161 << 16;

            if((i < outInts / 2 - 1) || ((length-1) % 32) >= 16){
                const int inindex2 = inindextrafo(i*2 + 1);

                const unsigned int data2 = in[inindex2];
                const unsigned int even162 = SequenceHelpers::extractEvenBits(data2);
                const unsigned int odd162 = SequenceHelpers::extractEvenBits(data2 >> 1);

                resultHi = resultHi | odd162;
                resultLo = resultLo | even162;
            }

            outHi[outIndex] = resultHi;
            outLo[outIndex] = resultLo;
        }
    };

    auto group = cg::tiled_partition<1>(cg::this_thread_block());
    const int numGroups = (blockDim.x * gridDim.x) / group.size();
    const int groupId = (threadIdx.x + blockIdx.x * blockDim.x) / group.size();

    for(int index = groupId; index < numSequences; index += numGroups){
        const int sequenceLength = sequenceLengths[index];
        const unsigned int* const in = inputdata + inputStartIndex(index);
        unsigned int* const out = outputdata + outputStartIndex(index);            

        convert(
            group,
            out,
            in,
            sequenceLength,
            inputTrafo,
            outputTrafo
        );
    } 
}



__global__
void convert2BitTo2BitHiloKernelTT(
        const unsigned int* const __restrict__ inputdata,
        size_t inputpitchInInts, // max num ints per input sequence
        unsigned int*  const __restrict__ outputdata,
        size_t outputpitchInInts, // max num ints per output sequence
        const int* const __restrict__ sequenceLengths,
        const int* __restrict__ numSequencesPtr){

    const int numSequences = *numSequencesPtr;

    auto inputStartIndex = [&](auto i){return i;};
    auto outputStartIndex = [&](auto i){return i;};
    auto inputTrafo = [&](auto i){return i * numSequences;};
    auto outputTrafo = [&](auto i){return i * numSequences;};

    auto convert = [&](auto group,
                        unsigned int* out,
                        const unsigned int* in,
                        int length,
                        auto inindextrafo,
                        auto outindextrafo){

        const int inInts = SequenceHelpers::getEncodedNumInts2Bit(length);
        const int outInts = SequenceHelpers::getEncodedNumInts2BitHiLo(length);

        unsigned int* const outHi = out;
        unsigned int* const outLo = out + outindextrafo(outInts/2);

        for(int i = group.thread_rank(); i < outInts / 2; i += group.size()){
            const int outIndex = outindextrafo(i);
            const int inindex1 = inindextrafo(i*2);

            const unsigned int data1 = in[inindex1];
            const unsigned int even161 = SequenceHelpers::extractEvenBits(data1);
            const unsigned int odd161 = SequenceHelpers::extractEvenBits(data1 >> 1);

            unsigned int resultHi = odd161 << 16;
            unsigned int resultLo = even161 << 16;

            if((i < outInts / 2 - 1) || ((length-1) % 32) >= 16){
                const int inindex2 = inindextrafo(i*2 + 1);

                const unsigned int data2 = in[inindex2];
                const unsigned int even162 = SequenceHelpers::extractEvenBits(data2);
                const unsigned int odd162 = SequenceHelpers::extractEvenBits(data2 >> 1);

                resultHi = resultHi | odd162;
                resultLo = resultLo | even162;
            }

            outHi[outIndex] = resultHi;
            outLo[outIndex] = resultLo;
        }
    };

    auto group = cg::tiled_partition<1>(cg::this_thread_block());
    const int numGroups = (blockDim.x * gridDim.x) / group.size();
    const int groupId = (threadIdx.x + blockIdx.x * blockDim.x) / group.size();

    for(int index = groupId; index < numSequences; index += numGroups){
        const int sequenceLength = sequenceLengths[index];
        const unsigned int* const in = inputdata + inputStartIndex(index);
        unsigned int* const out = outputdata + outputStartIndex(index);            

        convert(
            group,
            out,
            in,
            sequenceLength,
            inputTrafo,
            outputTrafo
        );
    } 
}





void callConversionKernel2BitTo2BitHiLoNN(
        const unsigned int* d_inputdata,
        size_t inputpitchInInts,
        unsigned int* d_outputdata,
        size_t outputpitchInInts,
        const int* d_sequenceLengths,
        const int* d_numSequences,
        int /*maxNumSequences*/,
        cudaStream_t stream){

    
    constexpr int groupsize = 8;        
    constexpr int blocksize = 128;
    constexpr size_t smem = 0;
    
    int deviceId = 0;
    int numSMs = 0;
    int maxBlocksPerSM = 0;
    CUDACHECK(cudaGetDevice(&deviceId));
    CUDACHECK(cudaDeviceGetAttribute(&numSMs, cudaDevAttrMultiProcessorCount, deviceId));
    CUDACHECK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &maxBlocksPerSM,
        convert2BitTo2BitHiloKernelNN<groupsize>,
        blocksize, 
        smem
    ));

    const int maxBlocks = maxBlocksPerSM * numSMs;

    dim3 block(blocksize,1,1);
    //dim3 grid(std::min(maxBlocks, SDIV(maxNumSequences * groupsize, blocksize)), 1, 1);
    dim3 grid(maxBlocks, 1, 1);

    convert2BitTo2BitHiloKernelNN<groupsize><<<grid, block, 0, stream>>>(
        d_inputdata,
        inputpitchInInts,
        d_outputdata,
        outputpitchInInts,
        d_sequenceLengths,
        d_numSequences); CUDACHECKASYNC;

#ifdef DO_CHECK_CONVERSIONS        

    callCheckSequenceConversionKernelNN(d_inputdata,
        inputpitchInInts,
        d_outputdata,
        outputpitchInInts,
        d_sequenceLengths,
        numSequences,
        stream);

#endif

}

void callConversionKernel2BitTo2BitHiLoNT(
        const unsigned int* d_inputdata,
        size_t inputpitchInInts,
        unsigned int* d_outputdata,
        size_t outputpitchInInts,
        const int* d_sequenceLengths,
        const int* d_numSequences,
        int /*maxNumSequences*/,
        cudaStream_t stream){

    constexpr int blocksize = 128;
    constexpr size_t smem = 0;

    int deviceId = 0;
    int numSMs = 0;
    int maxBlocksPerSM = 0;
    CUDACHECK(cudaGetDevice(&deviceId));
    CUDACHECK(cudaDeviceGetAttribute(&numSMs, cudaDevAttrMultiProcessorCount, deviceId));
    CUDACHECK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &maxBlocksPerSM,
        convert2BitTo2BitHiloKernelNT,
        blocksize, 
        smem
    ));

    const int maxBlocks = maxBlocksPerSM * numSMs;

    dim3 block(blocksize,1,1);
    //dim3 grid(std::min(maxBlocks, SDIV(maxNumSequences, blocksize)), 1, 1);
    dim3 grid(maxBlocks, 1, 1);

    convert2BitTo2BitHiloKernelNT<<<grid, block, 0, stream>>>(
        d_inputdata,
        inputpitchInInts,
        d_outputdata,
        outputpitchInInts,
        d_sequenceLengths,
        d_numSequences); CUDACHECKASYNC;

#if 0    

    callCheckSequenceConversionKernelNT(d_inputdata,
        inputpitchInInts,
        d_outputdata,
        outputpitchInInts,
        d_sequenceLengths,
        numSequences,
        stream);

#endif

}

void callConversionKernel2BitTo2BitHiLoTT(
        const unsigned int* d_inputdata,
        size_t inputpitchInInts,
        unsigned int* d_outputdata,
        size_t outputpitchInInts,
        const int* d_sequenceLengths,
        const int* d_numSequences,
        int /*maxNumSequences*/,
        cudaStream_t stream){

    constexpr int blocksize = 128;
    constexpr size_t smem = 0;

    int deviceId = 0;
    int numSMs = 0;
    int maxBlocksPerSM = 0;
    CUDACHECK(cudaGetDevice(&deviceId));
    CUDACHECK(cudaDeviceGetAttribute(&numSMs, cudaDevAttrMultiProcessorCount, deviceId));
    CUDACHECK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &maxBlocksPerSM,
        convert2BitTo2BitHiloKernelTT,
        blocksize, 
        smem
    ));

    const int maxBlocks = maxBlocksPerSM * numSMs;

    dim3 block(blocksize,1,1);
    //dim3 grid(std::min(maxBlocks, SDIV(maxNumSequences, blocksize)), 1, 1);
    dim3 grid(maxBlocks, 1, 1);

    convert2BitTo2BitHiloKernelTT<<<grid, block, 0, stream>>>(
        d_inputdata,
        inputpitchInInts,
        d_outputdata,
        outputpitchInInts,
        d_sequenceLengths,
        d_numSequences); CUDACHECKASYNC;

#if 0            

    callCheckSequenceConversionKernelTT(d_inputdata,
        inputpitchInInts,
        d_outputdata,
        outputpitchInInts,
        d_sequenceLengths,
        numSequences,
        stream);

#endif 
        
}



}
}




