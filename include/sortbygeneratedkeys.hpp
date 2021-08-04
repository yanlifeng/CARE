#ifndef CARE_SORTBYGENERATEDKEYS_HPP
#define CARE_SORTBYGENERATEDKEYS_HPP

#include <hpc_helpers.cuh>
#include <gpu/cudaerrorcheck.cuh>

#include <cstdint>
#include <memory>
#include <numeric>
#include <algorithm>
#include <iostream>

#include <thrust/sort.h>

#ifdef __CUDACC__
#include <cub/cub.cuh>
#endif

/*
    KeyType KeyGenerator::operator()(IndexType i)  returns i-th key

    bool KeyComparator::operator()(KeyType l, KeyType r)
*/
template<class IndexType, class ValueType, class KeyGenerator, class KeyComparator>
bool sortValuesByGeneratedKeysViaIndicesHost(
    std::size_t memoryLimitBytes,
    ValueType* values,
    IndexType numValues,
    KeyGenerator keyGenerator,
    KeyComparator keyComparator
){
    using KeyType = decltype(keyGenerator(IndexType{0}));

    std::cerr << "sortValuesByGeneratedKeysViaIndicesHost \n";

    std::size_t sizeOfKeys = SDIV(sizeof(KeyType) * numValues, sizeof(std::size_t)) * sizeof(std::size_t);
    std::size_t sizeOfIndices = SDIV(sizeof(IndexType) * numValues, sizeof(std::size_t)) * sizeof(std::size_t);
    std::size_t sizeOfValues = SDIV(sizeof(ValueType) * numValues, sizeof(std::size_t)) * sizeof(std::size_t);

    std::size_t requiredBytes = std::max(sizeOfValues, sizeOfKeys) + std::size_t(sizeOfIndices) + sizeof(std::size_t);

    if(requiredBytes >= memoryLimitBytes){
        std::cerr << sizeOfValues << " " <<  sizeOfKeys << " " <<  sizeOfIndices << " " << memoryLimitBytes << "\n";
        return false;
    }

    auto buffer = std::make_unique<char[]>(sizeOfIndices + std::max(sizeOfValues, sizeOfKeys));
    IndexType* const indices = (IndexType*)(buffer.get());
    KeyType* const keys = (KeyType*)(((char*)indices) + sizeOfIndices);
    ValueType* const newValues = (ValueType*)(((char*)indices) + sizeOfIndices);

    helpers::CpuTimer timer1("extractkeys");

    for(IndexType i = 0; i < numValues; i++){
        keys[i] = keyGenerator(i);
    }

    timer1.stop();
    //timer1.print();

    helpers::CpuTimer timer2("sort indices");

    std::iota(indices, indices + numValues, IndexType(0));

    std::sort(
        indices, indices + numValues,
        [&](const auto& l, const auto& r){
            return keyComparator(keys[l], keys[r]);
        }
    );

    timer2.stop();
    //timer2.print();

    //keys are no longer used. their memory is reused by newValues

    helpers::CpuTimer timer3("permute");
    for(IndexType i = 0; i < numValues; i++){
        newValues[i] = values[indices[i]];
    }
    std::copy_n(newValues, numValues, values);
    //permute(offsetsBegin, indices.data(), indices.size());

    timer3.stop();
    //timer3.print();

    return true;
}




/*
    KeyType KeyGenerator::operator()(IndexType i)  returns i-th key

    bool KeyComparator::operator()(KeyType l, KeyType r)
*/
template<class IndexType, class ValueType, class KeyGenerator, class KeyComparator>
bool sortValuesByGeneratedKeysViaSortByKeyHost(
    std::size_t memoryLimitBytes,
    ValueType* values,
    IndexType numValues,
    KeyGenerator keyGenerator,
    KeyComparator keyComparator
){
    using KeyType = decltype(keyGenerator(IndexType{0}));

    std::cerr << "sortValuesByGeneratedKeysViaSortByKeyHost \n";

    std::size_t sizeOfKeys = SDIV(sizeof(KeyType) * numValues, sizeof(std::size_t)) * sizeof(std::size_t);
    std::size_t sizeOfValues = SDIV(sizeof(ValueType) * numValues, sizeof(std::size_t)) * sizeof(std::size_t);

    std::size_t requiredBytes = 2 * sizeOfKeys + sizeOfValues;

    std::cerr << "requiredBytes: " << requiredBytes << ", memoryLimitBytes: " << memoryLimitBytes << '\n';

    if(requiredBytes >= memoryLimitBytes){
        std::cerr << sizeOfValues << " " <<  sizeOfKeys << " " << memoryLimitBytes << "\n";
        return false;
    }

    auto buffer = std::make_unique<char[]>(sizeOfKeys);
    KeyType* const keys = (KeyType*)(buffer.get());

    helpers::CpuTimer timer1("extractkeys");

    for(IndexType i = 0; i < numValues; i++){
        keys[i] = keyGenerator(i);
    }

    timer1.stop();
    //timer1.print();

    helpers::CpuTimer timer2("sort by key");

    thrust::sort_by_key(keys, keys + numValues, values);

    timer2.stop();
    //timer2.print();

    return true;
}


#ifdef __CUDACC__

/*
    KeyType KeyGenerator::operator()(IndexType i)  returns i-th key

    bool KeyComparator::operator()(KeyType l, KeyType r)
*/
template<class IndexType, class ValueType, class KeyGenerator, class KeyComparator>
bool sortValuesByGeneratedKeysViaSortByKeyDevice(
    std::size_t memoryLimitBytes,
    ValueType* values,
    IndexType numValues,
    KeyGenerator keyGenerator,
    KeyComparator keyComparator
){
    using KeyType = decltype(keyGenerator(IndexType{0}));

    std::cerr << "sortValuesByGeneratedKeysViaSortByKeyDevice \n";

    if(std::size_t(std::numeric_limits<int>::max()) < std::size_t(numValues)){
        std::cerr << numValues << " > " << std::numeric_limits<int>::max() << "\n";
        return false;
    }

    if(std::size_t(std::numeric_limits<int>::max()) < std::size_t(numValues)){
        std::cerr << numValues << " > " << std::numeric_limits<int>::max() << "\n";
        return false;
    }

    std::size_t sizeOfKeys = SDIV(sizeof(KeyType) * numValues, sizeof(std::size_t)) * sizeof(std::size_t);
    std::size_t sizeOfValues = SDIV(sizeof(ValueType) * numValues, sizeof(std::size_t)) * sizeof(std::size_t);

    //check if keys can be materialized in memory
    if(sizeOfKeys >= memoryLimitBytes){
        std::cerr << sizeOfKeys << " " << memoryLimitBytes << "\n";
        return false;
    }

    // Need to explicitly instanciate radix sort for OffsetT = IndexType. 
    // The default API only uses OffsetT = int which may be insufficient to enumerate keys
    auto DeviceRadixSort_SortPairs = [](
        void* d_temp_storage, 
        std::size_t& temp_storage_bytes, 
        cub::DoubleBuffer<KeyType>& d_keys, 
        cub::DoubleBuffer<ValueType>& d_values,
        IndexType num_items,
        cudaStream_t stream
    ){
        return cub::DispatchRadixSort<false, KeyType, ValueType, IndexType>::Dispatch(
            d_temp_storage,
            temp_storage_bytes,
            d_keys,
            d_values,
            num_items,
            0,
            sizeof(KeyType) * 8,
            true,
            stream,
            false
        );
    };

    cub::DoubleBuffer<KeyType> d_keys_dbl{nullptr, nullptr};
    cub::DoubleBuffer<ValueType> d_values_dbl{nullptr, nullptr};

    std::size_t requiredCubSize = 0;

    cudaError_t cubstatus = DeviceRadixSort_SortPairs(
        nullptr,
        requiredCubSize,
        d_keys_dbl,
        d_values_dbl,
        numValues,
        (cudaStream_t)0
    );

    if(cubstatus != cudaSuccess) return false;

    void* temp_allocations[5]{};
    std::size_t temp_allocation_sizes[5]{};

    temp_allocation_sizes[0] = sizeOfKeys; // d_keys
    temp_allocation_sizes[1] = sizeOfKeys; // d_keys_temp
    temp_allocation_sizes[2] = sizeOfValues; // d_values
    temp_allocation_sizes[3] = sizeOfValues; // d_values_temp
    temp_allocation_sizes[4] = requiredCubSize;

    std::size_t temp_storage_bytes = 0;
    cubstatus = cub::AliasTemporaries(
        nullptr,
        temp_storage_bytes,
        temp_allocations,
        temp_allocation_sizes
    );
    if(cubstatus != cudaSuccess) return false;

    std::size_t freeMem,totalMem;
    CUDACHECK(cudaMemGetInfo(&freeMem, &totalMem));

    //std::cerr << "free gpu mem: " << freeMem << ", memoryLimitBytes: " << memoryLimitBytes << ", sizeOfKeys: " << sizeOfKeys << ", temp_storage_bytes: " << temp_storage_bytes << "\n";

    void* temp_storage = nullptr;
    if(freeMem > temp_storage_bytes){
        cudaMalloc(&temp_storage, temp_storage_bytes);
    }else if(freeMem + memoryLimitBytes - sizeOfKeys > temp_storage_bytes){
        cudaMallocManaged(&temp_storage, temp_storage_bytes);
        int deviceId = 0;
        cudaGetDevice(&deviceId);
        cudaMemAdvise(temp_storage, temp_storage_bytes, cudaMemAdviseSetAccessedBy, deviceId);      
    }else{
        return false;
    }

    if(cudaGetLastError() != cudaSuccess || temp_storage == nullptr){
        if(temp_storage != nullptr){
            cudaFree(temp_storage);
        }
        return false;
    }

    cubstatus = cub::AliasTemporaries(
        temp_storage,
        temp_storage_bytes,
        temp_allocations,
        temp_allocation_sizes
    );
    if(cubstatus != cudaSuccess){
        if(temp_storage != nullptr){
            cudaFree(temp_storage);
        }
        return false;
    }

    auto keys = std::make_unique<KeyType[]>(numValues);

    helpers::CpuTimer timer1("extractkeys");

    for(IndexType i = 0; i < numValues; i++){
        keys[i] = keyGenerator(i);
    }

    // {
    //     std::ofstream os("keys_2");
    //     os.write((char*)keys.get(), sizeof(KeyType) * numValues);
    // }

    timer1.stop();
    //timer1.print();

    d_keys_dbl = cub::DoubleBuffer<KeyType>{(KeyType*)temp_allocations[0], (KeyType*)temp_allocations[1]};
    d_values_dbl = cub::DoubleBuffer<ValueType>{(ValueType*)temp_allocations[2], (ValueType*)temp_allocations[3]};

    helpers::CpuTimer timer2("copy to device");

    CUDACHECK(cudaMemcpy(d_keys_dbl.Current(), keys.get(), sizeof(KeyType) * numValues, H2D));
    keys = nullptr;
    CUDACHECK(cudaMemcpy(d_values_dbl.Current(), values, sizeof(ValueType) * numValues, H2D));

    // {
    //     std::ofstream os("offsets_2");
    //     os.write((char*)keys.get(), sizeof(ValueType) * numValues);
    // }

    timer2.stop();
    //timer2.print();

    helpers::CpuTimer timer3("cub sort");

    cubstatus = DeviceRadixSort_SortPairs(
        temp_allocations[4],
        requiredCubSize,
        d_keys_dbl,
        d_values_dbl,
        numValues,
        (cudaStream_t)0
    );
    CUDACHECK(cudaDeviceSynchronize());

    if(cubstatus != cudaSuccess){
        std::cerr << "cub::DeviceRadixSort::SortPairs error: " << cudaGetErrorString(cubstatus) << "\n";
        cudaGetLastError();
        cudaFree(temp_storage);
        return false;
    }

    timer3.stop();
    //timer3.print();

    helpers::CpuTimer timer4("copy to host");
    CUDACHECK(cudaMemcpy(values, d_values_dbl.Current(), sizeof(ValueType) * numValues, D2H));

    cudaDeviceSynchronize();
    timer4.stop();
    //timer4.print();

    CUDACHECK(cudaFree(temp_storage));

    cudaError_t cudastatus = cudaDeviceSynchronize();

    if(cubstatus != cudaSuccess && cudastatus != cudaSuccess) return false;

    // {
    //     std::ofstream os("sortedoffsets_2");
    //     os.write((char*)values, sizeof(ValueType) * numValues);
    // }

    return true;
}


#endif


/*
    Sorts the values of key-value pairs by key. Keys are generated via functor
*/
template<class IndexType, class ValueType, class KeyGenerator, class KeyComparator>
bool sortValuesByGeneratedKeys(
    std::size_t memoryLimitBytes,
    ValueType* values,
    IndexType numValues,
    KeyGenerator keyGenerator,
    KeyComparator keyComparator
){

    bool success = false;

    #ifdef __CUDACC__
        try{
            success = sortValuesByGeneratedKeysViaSortByKeyDevice<IndexType>(memoryLimitBytes, values, numValues, keyGenerator, keyComparator);
        } catch (...){
            std::cerr << "Fallback\n";
        }

        if(success) return true;        
    #endif

    try{
        success = sortValuesByGeneratedKeysViaSortByKeyHost<IndexType>(memoryLimitBytes, values, numValues, keyGenerator, keyComparator);
    } catch (...){
        std::cerr << "Fallback\n";
    }

    if(success) return true;

    try{
        success = sortValuesByGeneratedKeysViaIndicesHost<IndexType>(memoryLimitBytes, values, numValues, keyGenerator, keyComparator);
    } catch (...){
        std::cerr << "Fallback\n";
    }

    return success;
}




#endif