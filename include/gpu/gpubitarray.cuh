#ifndef CARE_GPUBITARRAY_CUH
#define CARE_GPUBITARRAY_CUH

#include <gpu/nvtxtimelinemarkers.hpp>

#include <hpc_helpers.cuh>
#include <config.hpp>
#include <cassert>

namespace care{

enum class BitArrayLocation {Host, Device};

template<class Index_t, BitArrayLocation location>
struct BitArrayBase{
    using Data_t = unsigned int;    
    
    Data_t* data = nullptr;
    size_t numBits = 0;

    HOSTDEVICEQUALIFIER
    bool isSetNonatomic(Index_t position) const{
        assert(position < numBits);

        const size_t dataIndex = position / (8 * sizeof(Data_t));
        const size_t bitIndex = position % (8 * sizeof(Data_t));

        return (data[dataIndex] >> bitIndex) & 1;
    }

    HOSTDEVICEQUALIFIER
    void set(Index_t position, bool value){
        #ifdef __CUDA_ARCH__
            auto CAS = [](auto addr, auto old, auto newVal){
                return atomicCAS(addr, old, newVal);
            };
        #else 
            auto CAS = [](auto addr, auto old, auto newVal){
                return __sync_val_compare_and_swap(addr, old, newVal);
            };
        #endif 

        const size_t dataIndex = position / (8 * sizeof(Data_t));
        const size_t bitIndex = position % (8 * sizeof(Data_t));

        Data_t* const address = &data[dataIndex];
        Data_t old = *address;
        Data_t assumed = 0;

        do {
            assumed = old;
            const Data_t newVal = (old & ~(1UL << bitIndex)) | (value << bitIndex);
            old = CAS(address, assumed, newVal);
        } while (assumed != old);
    }
};

template<class Index_t>
using CpuBitArray = BitArrayBase<Index_t, BitArrayLocation::Host>;


template<class Index_t>
CpuBitArray<Index_t> makeCpuBitArray(size_t numBits){    
    using Data_t = typename CpuBitArray<Index_t>::Data_t;

    CpuBitArray<Index_t> array;
    
    const size_t dataElements = SDIV(numBits, 8 * sizeof(Data_t));
    array.data = new Data_t[dataElements];
    array.numBits = numBits;

    std::fill_n(array.data, dataElements, 0);

    return array;
}

template<class Index_t>
void destroyCpuBitArray(CpuBitArray<Index_t>& array){  
    delete [] array.data;  
    array.data = nullptr;
    array.numBits = 0;
}

#ifdef __CUDACC__

template<class Index_t>
using GpuBitArray = BitArrayBase<Index_t, BitArrayLocation::Device>;

template<class Index_t>
GpuBitArray<Index_t> makeGpuBitArray(size_t numBits){    
    using Data_t = typename GpuBitArray<Index_t>::Data_t;

    GpuBitArray<Index_t> array;
    
    const size_t dataElements = SDIV(numBits, 8 * sizeof(Data_t));
    cudaMalloc(&array.data, sizeof(Data_t) * dataElements); CUERR;
    array.numBits = numBits;

    cudaMemset(array.data, 0, dataElements); CUERR;

    return array;
}

template<class Index_t>
GpuBitArray<Index_t> makeGpuBitArrayFrom(const CpuBitArray<Index_t>& other){
    using Data_t = typename GpuBitArray<Index_t>::Data_t;

    GpuBitArray<Index_t> array;
    
    const size_t dataElements = SDIV(other.numBits, 8 * sizeof(Data_t));
    cudaMalloc(&array.data, sizeof(Data_t) * dataElements); CUERR;
    array.numBits = other.numBits;

    cudaMemcpy(array.data, other.data, sizeof(Data_t) * dataElements, H2D); CUERR

    return array;
}

template<class Index_t>
void destroyGpuBitArray(GpuBitArray<Index_t>& array){  
    cudaFree(array.data); CUERR;
    array.data = nullptr;
    array.numBits = 0;
}


template<class Index_t>
__global__
void readBitarray(bool* result, GpuBitArray<Index_t> bitarray, const Index_t* positions, int nPositions){
    const int tid = threadIdx.x + blockIdx.x * blockDim.x;
    const int stride = blockDim.x * gridDim.x;

    for(int index = tid; index < nPositions; index += stride){

        const Index_t position = positions[index];

        result[index] = bitarray.isSetNonatomic(position);
    }
}

template<class Index_t>
__global__
void setBitarray(GpuBitArray<Index_t> bitarray, bool* values, const Index_t* positions, int nPositions){
    const int tid = threadIdx.x + blockIdx.x * blockDim.x;
    const int stride = blockDim.x * gridDim.x;

    for(int index = tid; index < nPositions; index += stride){

        const Index_t position = positions[index];
        const bool value = values[index];
        bitarray.set(position, value);

        //assert(value == bitarray.isSetNonatomic(position));
    }
}



#endif







}




#endif