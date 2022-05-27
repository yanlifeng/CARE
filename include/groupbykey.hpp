#ifndef CARE_GROUP_BY_KEY_HPP
#define CARE_GROUP_BY_KEY_HPP

#include <hpc_helpers.cuh>
#include <gpu/cudaerrorcheck.cuh>

#include <thrust/system/omp/execution_policy.h>
#include <thrust/iterator/constant_iterator.h>
#include <thrust/inner_product.h>
#include <thrust/sequence.h>
#include <thrust/copy.h>
#include <thrust/sort.h>
#include <thrust/equal.h>

#ifdef __NVCC__
#include <thrust/device_vector.h>
#include <gpu/gpuhashtable.cuh>
#include <cooperative_groups.h>
#include <cub/cub.cuh>

#include <gpu/cudaerrorcheck.cuh>

#include <rmm/mr/device/device_memory_resource.hpp>
#include <rmm/mr/device/per_device_resource.hpp>
#include <rmm/mr/device/cuda_memory_resource.hpp>
#include <rmm/mr/device/managed_memory_resource.hpp>
#include <rmm/mr/device/limiting_resource_adaptor.hpp>
#include <rmm/device_uvector.hpp>
#include <gpu/rmm_utilities.cuh>
#endif


#ifdef CARE_HAS_WARPCORE

#include <warpcore/multi_value_hash_table.cuh>

#endif


#include <vector>
#include <cassert>
#include <cstddef>
#include <iostream>



namespace care{

    template<class Key_t, class Value_t, class Offset_t>
    struct GroupByKeyCpu{
        bool valuesOfSameKeyMustBeSorted = false;
        int maxValuesPerKey = 0;
        int minValuesPerKey = 0;

        GroupByKeyCpu(bool sortValues, int maxValuesPerKey_, int minValuesPerKey_) 
            : valuesOfSameKeyMustBeSorted(sortValues), 
                maxValuesPerKey(maxValuesPerKey_),
                minValuesPerKey(minValuesPerKey_){}

        /*
            Input: keys and values. keys[i] and values[i] form a key-value pair
            Output: unique keys. values with the same key are stored consecutive. values of unique_keys[i] are
            stored at values[offsets[i]] to values[offsets[i+1]] (exclusive)
            If valuesOfSameKeyMustBeSorted == true, values with the same key are sorted in ascending order.
            If there are more than maxValuesPerKey values with the same key, all of those values are removed,
            i.e. the key ends up with 0 values
        */
        void execute(std::vector<Key_t>& keys, std::vector<Value_t>& values, std::vector<Offset_t>& offsets){
            if(keys.size() == 0){
                //deallocate unused memory if capacity > 0
                keys = std::vector<Key_t>{};
                values = std::vector<Value_t>{};
                return;
            }

            if(valuesOfSameKeyMustBeSorted){
                const bool isIotaValues = checkIotaValues(values);
                if(!isIotaValues)
                    throw std::runtime_error("Error hashtable compaction");
            }

            executeWithIotaValues(keys, values, offsets);
        }

        bool checkIotaValues(const std::vector<Value_t>& values){
            auto policy = thrust::host;

            bool isIotaValues = thrust::equal(
                policy,
                thrust::counting_iterator<Value_t>{0},
                thrust::counting_iterator<Value_t>{0} + values.size(),
                values.data()
            ); 

            return isIotaValues;
        }

        void executeWithIotaValues(std::vector<Key_t>& keys, std::vector<Value_t>& values, std::vector<Offset_t>& offsets){
            assert(keys.size() == values.size()); //key value pairs
            assert(std::numeric_limits<Offset_t>::max() >= keys.size()); //total number of keys must fit into Offset_t

            auto deallocVector = [](auto& vec){
                using T = typename std::remove_reference<decltype(vec)>::type;
                T tmp{};
                vec.swap(tmp);
            };

            deallocVector(offsets); //don't need offsets at the moment

            const std::size_t size = keys.size();
            auto policy = thrust::omp::par;

            if(valuesOfSameKeyMustBeSorted){
                auto kb = keys.data();
                auto ke = keys.data() + size;
                auto vb = values.data();

                thrust::stable_sort_by_key(policy, kb, ke, vb);
            }else{
                auto kb = keys.data();
                auto ke = keys.data() + size;
                auto vb = values.data();

                thrust::sort_by_key(policy, kb, ke, vb);
            }

            const Offset_t nUniqueKeys = thrust::inner_product(policy,
                                            keys.begin(),
                                            keys.end() - 1,
                                            keys.begin() + 1,
                                            Offset_t(1),
                                            thrust::plus<Key_t>(),
                                            thrust::not_equal_to<Key_t>());

            //std::cout << "unique keys " << nUniqueKeys << ". ";

            std::vector<Key_t> uniqueKeys(nUniqueKeys);
            std::vector<Offset_t> valuesPerKey(nUniqueKeys);

            auto* keys_begin = keys.data();
            auto* keys_end = keys.data() + size;
            auto* uniqueKeys_begin = uniqueKeys.data();
            auto* valuesPerKey_begin = valuesPerKey.data();

            //make histogram
            auto histogramEndIterators = thrust::reduce_by_key(
                policy,
                keys_begin,
                keys_end,
                thrust::constant_iterator<Offset_t>(1),
                uniqueKeys_begin,
                valuesPerKey_begin
            );

            if(histogramEndIterators.first != uniqueKeys.data() + nUniqueKeys)
                throw std::runtime_error("Error hashtable compaction");
            if(histogramEndIterators.second != valuesPerKey.data() + nUniqueKeys)
                throw std::runtime_error("Error hashtable compaction");

            keys.swap(uniqueKeys);
            deallocVector(uniqueKeys);

            offsets.resize(nUniqueKeys+1);
            offsets[0] = 0;

            thrust::inclusive_scan(
                policy,
                valuesPerKey_begin,
                valuesPerKey_begin + nUniqueKeys,
                offsets.data() + 1
            );

            std::vector<char> removeflags(values.size(), false);

            thrust::for_each(
                policy,
                thrust::counting_iterator<Offset_t>(0),
                thrust::counting_iterator<Offset_t>(0) + nUniqueKeys,
                [&](Offset_t index){
                    const auto begin = offsets[index];
                    const auto end = offsets[index+1];
                    const auto num = end - begin;

                    if(num > Offset_t(maxValuesPerKey) || num < Offset_t(minValuesPerKey)){
                        valuesPerKey_begin[index] = 0;

                        for(Offset_t k = begin; k < end; k++){
                            removeflags[k] = true;
                        }
                    }
                }
            );

            deallocVector(offsets);    

            Offset_t numValuesToRemove = thrust::reduce(
                policy,
                removeflags.begin(),
                removeflags.end(),
                Offset_t(0)
            );

            std::vector<Value_t> values_tmp(size - numValuesToRemove);

            thrust::copy_if(
                values.begin(),
                values.end(),
                removeflags.begin(),
                values_tmp.begin(),
                [](auto flag){
                    return flag == 0;
                }
            );

            values.swap(values_tmp);
            deallocVector(values_tmp);

            offsets.resize(nUniqueKeys+1);
            offsets[0] = 0;

            thrust::inclusive_scan(
                policy,
                valuesPerKey_begin,
                valuesPerKey_begin + nUniqueKeys,
                offsets.data() + 1
            );
        }
    };

    #ifdef __NVCC__

    template<class Key_t, class Value_t, class Offset_t>
    struct GroupByKeyGpu{
        //gpu allocator which uses cudaMallocManaged if not enough gpu memory is available for cudaMalloc
        template<class T>
        using ThrustAlloc = helpers::ThrustFallbackDeviceAllocator<T, true>;


        bool valuesOfSameKeyMustBeSorted = false;
        int maxValuesPerKey = 0;
        int minValuesPerKey = 0;

        GroupByKeyGpu(bool sortValues, int maxValuesPerKey_, int minValuesPerKey_) 
            : valuesOfSameKeyMustBeSorted(sortValues), 
                maxValuesPerKey(maxValuesPerKey_),
                minValuesPerKey(minValuesPerKey_){}

        /*
            Input: keys and values. keys[i] and values[i] form a key-value pair
            Output: unique keys. values with the same key are stored consecutive. values of unique_keys[i] are
            stored at values[offsets[i]] to values[offsets[i+1]] (exclusive)
            If valuesOfSameKeyMustBeSorted == true, values with the same key are sorted in ascending order.
            If there are more than maxValuesPerKey values with the same key, all of those values are removed,
            i.e. the key ends up with 0 values
        */
        bool execute(std::vector<Key_t>& keys, std::vector<Value_t>& values, std::vector<Offset_t>& offsets){
            if(keys.size() == 0){
                //deallocate unused memory if capacity > 0
                keys = std::vector<Key_t>{};
                values = std::vector<Value_t>{};
                return true;
            }

            bool success = false;

            if(valuesOfSameKeyMustBeSorted){
                bool isIotaValues = checkIotaValues(values);
                assert(isIotaValues);
            }

            //if(isIotaValues){                   
                try{           
                    executeWithIotaValues(keys, values, offsets);
                    success = true;
                }catch(const thrust::system_error& ex){
                    std::cerr << ex.what() << '\n';
                    cudaGetLastError();
                    success = false;
                }catch(const std::exception& ex){
                    std::cerr << ex.what() << '\n';
                    cudaGetLastError();
                    success = false;
                }catch(...){
                    cudaGetLastError();
                    success = false;
                }                    
            //}else{
            //    assert(false && "not implemented");
            //}

            return success;
        }

        bool checkIotaValues(const std::vector<Value_t>& values){
            auto policy = thrust::host;

            nvtx::push_range("checkIotaValues", 6);

            bool isIotaValues = thrust::equal(
                policy,
                thrust::counting_iterator<Value_t>{0},
                thrust::counting_iterator<Value_t>{0} + values.size(),
                values.data()
            ); 

            nvtx::pop_range();

            return isIotaValues;
        }

        void executeWithIotaValues(std::vector<Key_t>& keys, std::vector<Value_t>& values, std::vector<Offset_t>& offsets){
            assert(keys.size() == values.size()); //key value pairs
            assert(std::numeric_limits<Offset_t>::max() >= keys.size()); //total number of keys must fit into Offset_t

            auto deallocVector = [](auto& vec){
                using T = typename std::remove_reference<decltype(vec)>::type;
                T tmp{};
                vec.swap(tmp);
            };

            rmm::mr::device_memory_resource* currentResource = rmm::mr::get_current_device_resource();
            rmm::mr::limiting_resource_adaptor limitedCurrentResource(currentResource, 0);
            rmm::mr::managed_memory_resource managedMemoryResource;
            rmm::mr::FallbackResourceAdapter fallbackResource(currentResource, &managedMemoryResource);
            rmm::mr::device_memory_resource* mr = &fallbackResource;

            cudaStream_t stream = 0;

            deallocVector(offsets); //don't need offsets at the moment

            const std::size_t size = keys.size();

            auto thrustPolicy = rmm::exec_policy_nosync(stream, mr);

            rmm::device_uvector<Key_t> d_keys(size, stream, mr);
            rmm::device_uvector<Value_t> d_values(size, stream, mr);

            CUDACHECK(cudaMemcpyAsync(
                d_keys.data(),
                keys.data(),
                sizeof(Key_t) * size,
                H2D,
                stream
            ));

            //no need to transfer iota values. generate them on the device
            thrust::sequence(
                thrustPolicy,
                d_values.begin(),
                d_values.end(),
                Value_t(0)
            );

            if(valuesOfSameKeyMustBeSorted){
                thrust::stable_sort_by_key(thrustPolicy, d_keys.begin(), d_keys.end(), d_values.begin());
            }else{
                thrust::sort_by_key(thrustPolicy, d_keys.begin(), d_keys.end(), d_values.begin());
            }

            CUDACHECK(cudaMemcpyAsync(
                values.data(),
                d_values.data(),
                sizeof(Value_t) * size,
                D2H,
                stream
            ));

            ::destroy(d_values, stream);

            const Offset_t nUniqueKeys = thrust::inner_product(
                thrustPolicy,
                d_keys.begin(),
                d_keys.end() - 1,
                d_keys.begin() + 1,
                Offset_t(1),
                thrust::plus<Key_t>(),
                thrust::not_equal_to<Key_t>()
            );

            //std::cout << "unique keys " << nUniqueKeys << ". ";

            rmm::device_uvector<Key_t> d_uniqueKeys(nUniqueKeys, stream, mr);
            rmm::device_uvector<Offset_t> d_valuesPerKey(nUniqueKeys, stream, mr);

            //make histogram
            auto histogramEndIterators = thrust::reduce_by_key(
                thrustPolicy,
                d_keys.begin(),
                d_keys.end(),
                thrust::constant_iterator<Offset_t>(1),
                d_uniqueKeys.begin(),
                d_valuesPerKey.begin()
            );

            ::destroy(d_keys, stream);

            if(histogramEndIterators.first != d_uniqueKeys.begin() + nUniqueKeys)
                throw std::runtime_error("Error hashtable compaction");
            if(histogramEndIterators.second != d_valuesPerKey.begin() + nUniqueKeys)
                throw std::runtime_error("Error hashtable compaction");

            deallocVector(keys);
            keys.resize(nUniqueKeys);

            CUDACHECK(cudaMemcpyAsync(
                keys.data(),
                d_uniqueKeys.data(),
                sizeof(Key_t) * nUniqueKeys,
                D2H,
                stream
            ));
            
            ::destroy(d_uniqueKeys, stream);

            rmm::device_uvector<Offset_t> d_offsets(nUniqueKeys + 1, stream, mr);

            CUDACHECK(cudaMemsetAsync(
                d_offsets.data(),
                0,
                sizeof(Offset_t),
                stream
            ));

            thrust::inclusive_scan(
                thrustPolicy,
                d_valuesPerKey.begin(),
                d_valuesPerKey.end(),
                d_offsets.begin() + 1
            );
            
            rmm::device_uvector<char> d_removeflags(size, stream, mr);
            CUDACHECK(cudaMemsetAsync(
                d_removeflags.data(),
                0,
                sizeof(char) * size,
                stream
            ));


            auto d_removeflags_begin = d_removeflags.data();
            auto d_offsets_begin = d_offsets.data();
            auto d_valuesPerKey_begin = d_valuesPerKey.data();
            auto maxValuesPerKey_copy = maxValuesPerKey;
            auto minValuesPerKey_copy = minValuesPerKey;
            thrust::for_each(
                thrustPolicy,
                thrust::counting_iterator<Offset_t>(0),
                thrust::counting_iterator<Offset_t>(0) + nUniqueKeys,
                [=] __device__ (Offset_t index){
                    const auto begin = d_offsets_begin[index];
                    const auto end = d_offsets_begin[index+1];
                    const auto num = end - begin;

                    if(num > Offset_t(maxValuesPerKey_copy) || num < Offset_t(minValuesPerKey_copy)){
                        d_valuesPerKey_begin[index] = 0;

                        for(Offset_t k = begin; k < end; k++){
                            d_removeflags_begin[k] = true;
                        }
                    }
                }
            );

            ::destroy(d_offsets, stream);

            Offset_t numValuesToRemove = thrust::reduce(
                thrustPolicy,
                d_removeflags.begin(),
                d_removeflags.end(),
                Offset_t(0)
            );

            rmm::device_uvector<Value_t> d_values_tmp(size - numValuesToRemove, stream, mr);
            d_values.resize(values.size(), stream);
            CUDACHECK(cudaMemcpyAsync(
                d_values.data(),
                values.data(),
                sizeof(Value_t) * size,
                H2D,
                stream
            ));

            thrust::copy_if(
                thrustPolicy,
                d_values.begin(),
                d_values.end(),
                d_removeflags.begin(),
                d_values_tmp.begin(),
                [] __device__ (auto flag){
                    return flag == 0;
                }
            );

            ::destroy(d_removeflags, stream);
            ::destroy(d_values, stream);
            deallocVector(values);
            values.resize(size - numValuesToRemove);

            CUDACHECK(cudaMemcpyAsync(
                values.data(),
                d_values_tmp.data(),
                sizeof(Value_t) * d_values_tmp.size(),
                D2H,
                stream
            ));

            offsets.resize(nUniqueKeys+1);               
            d_offsets.resize(nUniqueKeys, stream);

            thrust::inclusive_scan(
                thrustPolicy,
                d_valuesPerKey.begin(),
                d_valuesPerKey.end(),
                d_offsets.begin()
            );

            CUDACHECK(cudaMemcpyAsync(
                offsets.data() + 1,
                d_offsets.data(),
                sizeof(Offset_t) * nUniqueKeys,
                D2H,
                stream
            ));

            offsets[0] = 0;

            CUDACHECK(cudaStreamSynchronize(stream));
        }
    };
     
    #endif //#ifdef __NVCC__


}


#endif