#ifndef CARE_MINHASHKERNELS_HPP
#define CARE_MINHASHKERNELS_HPP

#include <config.hpp>
#include <hpc_helpers.cuh>
#include <gpu/simpleallocation.cuh>

#include <array>
#include <vector>

namespace care{


template<class T>
struct MergeRangesGpuHandle{
    SimpleAllocationDevice<T> d_data;
    SimpleAllocationPinnedHost<T> h_data;
    
    SimpleAllocationPinnedHost<int> h_rangesBeginPerSequence;
    SimpleAllocationDevice<int> d_rangesBeginPerSequence;

    SimpleAllocationDevice<T> d_results;
    SimpleAllocationPinnedHost<T> h_results;

    SimpleAllocationPinnedHost<int> h_numresults;

    SimpleAllocationPinnedHost<int> h_num_runs;
    SimpleAllocationDevice<int> d_num_runs;

    SimpleAllocationPinnedHost<int> h_uniqueRangeLengths;
    SimpleAllocationDevice<int> d_uniqueRangeLengths;

    SimpleAllocationDevice<int> d_uniqueRangeLengthsPrefixsum;

    SimpleAllocationDevice<char> cubTempStorage;

    int initialDataSize = 0;

    std::array<cudaStream_t, 1> streams{};
    std::array<cudaEvent_t, 2> events{};    
};

template<class T>
MergeRangesGpuHandle<T> makeMergeRangesGpuHandle(){
    MergeRangesGpuHandle<T> handle;
    for(auto& pipelinestream : handle.streams){
        cudaStreamCreate(&pipelinestream); CUERR;
    }

    for(auto& event : handle.events){
        cudaEventCreateWithFlags(&event, cudaEventDisableTiming); CUERR;
    }  

    return handle;
}


template<class T>
void destroyMergeRangesGpuHandle(MergeRangesGpuHandle<T>& handle){

    handle.d_data.destroy();
    handle.h_data.destroy();
    handle.d_rangesBeginPerSequence.destroy();
    handle.h_rangesBeginPerSequence.destroy();
    handle.d_results.destroy();
    handle.h_results.destroy();
    handle.h_numresults.destroy();
    handle.d_num_runs.destroy();
    handle.h_num_runs.destroy();
    handle.d_uniqueRangeLengths.destroy();
    handle.h_uniqueRangeLengths.destroy();
    handle.d_uniqueRangeLengthsPrefixsum.destroy();
    handle.cubTempStorage.destroy();

    for(auto& pipelinestream : handle.streams){
        cudaStreamDestroy(pipelinestream); CUERR;
    }

    for(auto& event : handle.events){
        cudaEventDestroy(event); CUERR;
    }  
}




struct OperationResult{
    std::vector<read_number> candidateIds;
    std::vector<int> candidateIdsPerSequence;
};

enum class MergeRangesKernelType{
    devicewide,
    allcub,
    popcmultiwarp,
    popcsinglewarp,
    popcsinglewarpchunked,
};


OperationResult mergeRangesGpu(
        MergeRangesGpuHandle<read_number>& handle, 
        const std::pair<const read_number*, const read_number*>* ranges, 
        int numRanges, 
        int rangesPerSequence, 
        cudaStream_t stream,
        MergeRangesKernelType kernelType);














} //namespace care


#endif
