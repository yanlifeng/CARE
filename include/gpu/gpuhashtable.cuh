#ifdef CARE_HAS_WARPCORE

#ifndef CARE_GPUHASHTABLE_CUH
#define CARE_GPUHASHTABLE_CUH

#include <warpcore/single_value_hash_table.cuh>
#include <warpcore/multi_value_hash_table.cuh>
#include <warpcore/multi_bucket_hash_table.cuh>

#include <hpc_helpers.cuh>
#include <cpuhashtable.hpp>
#include <memorymanagement.hpp>
#include <gpu/cudaerrorcheck.cuh>

#include <memory>
#include <algorithm>
#include <numeric>
#include <limits>
#include <cassert>
#include <future>

#include <cooperative_groups.h>
#include <cub/cub.cuh>

#include <thrust/execution_policy.h>
#include <thrust/sequence.h>
#include <thrust/iterator/discard_iterator.h>

namespace cg = cooperative_groups;

namespace care{
namespace gpu{

    namespace gpuhashtablekernels{

        template<class T, class U>
        __global__
        void assignmentKernel(T* __restrict__ output, const U* __restrict__ input, int N){
            const int tid = threadIdx.x + blockIdx.x * blockDim.x;
            const int stride = blockDim.x * gridDim.x;

            for(int i = tid; i < N; i += stride){
                output[i] = input[i];
            }
        }

        template<class T, class IsValidKey>
        __global__
        void fixTableKeysKernel(T* __restrict__ keys, int numKeys, IsValidKey isValid){
            const int tid = threadIdx.x + blockIdx.x * blockDim.x;
            const int stride = blockDim.x * gridDim.x;

            for(int i = tid; i < numKeys; i += stride){
                T key = keys[i];
                int changed = 0;
                while(!isValid(key)){
                    key++;
                    changed = 1;
                }
                if(changed == 1){
                    keys[i] = key;
                }
            }
        }

        //insert (key[i], firstValue + i)
        template<class Core, class Key, class Value>
        __global__
        void insertIotaValuesKernel(
            Core table,
            const Key* __restrict__ keys,
            Value firstValue,
            int numKeys
        ){
            const int tid = threadIdx.x + blockDim.x * blockIdx.x;
            const int stride = blockDim.x * gridDim.x;

            constexpr int tilesize = Core::cg_size();

            auto tile = cg::tiled_partition<tilesize>(cg::this_thread_block());
            const int tileId = tid / tilesize;
            const int numTiles = stride / tilesize;

            for(int k = tileId; k < numKeys; k += numTiles){
                const Key key = keys[k];
                const Value value = firstValue + static_cast<Value>(k);

                constexpr auto probing_length = warpcore::defaults::probing_length();

                const auto status =
                    table.insert(key, value, tile, probing_length);

                //ignore status
                (void)status;
            }
        }

        template<class Core, class Key, class Value>
        void callInsertIotaValuesKernel(
            Core table,
            const Key* d_keys,
            Value firstValue,
            int num,
            cudaStream_t stream
        ){
            auto kernel = insertIotaValuesKernel<Core, Key, Value>;

            constexpr int blocksize = 512;
            int deviceId = 0;
            int numSMs = 0;
            int maxBlocksPerSM = 0;
            CUDACHECK(cudaGetDevice(&deviceId));
            CUDACHECK(cudaDeviceGetAttribute(&numSMs, cudaDevAttrMultiProcessorCount, deviceId));
            CUDACHECK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
                &maxBlocksPerSM,
                kernel,
                blocksize, 
                0
            ));
        
            const int maxBlocks = maxBlocksPerSM * numSMs;  
        
            dim3 block(blocksize, 1, 1);

            constexpr int groupsize = Core::cg_size();


            const int numBlocks = SDIV(num, blocksize / groupsize);
            dim3 grid(std::min(numBlocks, maxBlocks), 1, 1);

            insertIotaValuesKernel<<<grid, block, 0, stream>>>(
                table, 
                d_keys,
                firstValue,
                num
            );
            CUDACHECKASYNC;
        }

        template<class DeviceTableView, class Key, class Value, class Offset>
        __global__
        void retrieveCompactKernel(
            DeviceTableView table,
            const Key* __restrict__ querykeys,
            const Offset* __restrict__ beginOffsets,
            const int* __restrict__ numValuesPerKey,
            const int /*maxValuesPerKey*/,
            const int numKeys,
            Value* __restrict__ outValues
        ){
            const int tid = threadIdx.x + blockDim.x * blockIdx.x;
            const int stride = blockDim.x * gridDim.x;

            constexpr int tilesize = DeviceTableView::cg_size();

            assert(stride % tilesize == 0);

            auto tile = cg::tiled_partition<tilesize>(cg::this_thread_block());
            const int tileId = tid / tilesize;
            const int numTiles = stride / tilesize;

            for(int k = tileId; k < numKeys; k += numTiles){
                const Key key = querykeys[k];
                const auto beginOffset = beginOffsets[k];
                const int num = numValuesPerKey[k];

                if(num != 0){
                    table.retrieve(tile, key, outValues + beginOffset);
                }
            }
        }


        template<class DeviceTableView, class Key, class Offset>
        __global__
        void numValuesPerKeyCompactKernel(
            const DeviceTableView table,
            int maxValuesPerKey,
            const Key* const __restrict__ querykeys,
            const int numKeys,
            Offset* const __restrict__ numValuesPerKey
        ){
            const int tid = threadIdx.x + blockDim.x * blockIdx.x;
            const int stride = blockDim.x * gridDim.x;

            constexpr int tilesize = DeviceTableView::cg_size();

            assert(stride % tilesize == 0);

            auto tile = cg::tiled_partition<tilesize>(cg::this_thread_block());
            const int tileId = tid / tilesize;
            const int numTiles = stride / tilesize;

            for(int k = tileId; k < numKeys; k += numTiles){
                const Key key = querykeys[k];

                const int num = table.numValues(tile, key);
                if(tile.thread_rank() == 0){
                    numValuesPerKey[k] = num > maxValuesPerKey ? 0 : num;
                    //numValuesPerKey[k] = num;
                }    
            }
        }


        //query the same number of keys in multiple tables
        //The output buffer of values is shared among all tables. the destination offset within the buffer is given by beginOffsets
        //This kernel expects a 2D grid of thread blocks. y dimension selects the table
        template<class DeviceTableView, class Key, class Value, class Offset>
        __global__
        void retrieveCompactKernel(
            const DeviceTableView* __restrict__ tables,
            const int numTables,
            const Key* __restrict__ querykeys,
            const int querykeysPitchInElements,
            const Offset* __restrict__ beginOffsets,
            const int beginOffsetsPitchInElements,
            const int* __restrict__ numValuesPerKey,
            const int numValuesPerKeyPitchInElements,
            const int /*maxValuesPerKey*/,
            const int numKeys,
            Value* __restrict__ outValues
        ){
            const int tid = threadIdx.x + blockDim.x * blockIdx.x;
            const int stride = blockDim.x * gridDim.x;

            constexpr int tilesize = DeviceTableView::cg_size();

            assert(stride % tilesize == 0);

            auto tile = cg::tiled_partition<tilesize>(cg::this_thread_block());
            const int tileId = tid / tilesize;
            const int numTiles = stride / tilesize;

            for(int tableid = blockIdx.y; tableid < numTables; tableid += gridDim.y){

                const DeviceTableView table = tables[tableid];
                const Key* const myQueryKeys = querykeys + querykeysPitchInElements * tableid;
                const int* const myNumValuesPerKey = numValuesPerKey + numValuesPerKeyPitchInElements * tableid;
                const Offset* const myBeginOffsets = beginOffsets + beginOffsetsPitchInElements * tableid;
                //Value* const myOutValues = outValues + outValuesPitchInElements * tableid;

                for(int k = tileId; k < numKeys; k += numTiles){
                    const Key key = myQueryKeys[k];
                    const auto beginOffset = myBeginOffsets[k];
                    const int num = myNumValuesPerKey[k];

                    if(num != 0){
                        table.retrieve(tile, key, outValues + beginOffset);
                    }
                }

            }
        }

        //query the same number of keys in multiple tables
        //This kernel expects a 2D grid of thread blocks. y dimension selects the table
        template<class DeviceTableView, class Key, class Offset>
        __global__
        void numValuesPerKeyCompactMultiTableKernel(
            const DeviceTableView* __restrict__ tables,
            const int numTables,
            const int maxValuesPerKey,
            const Key* const __restrict__ querykeys,
            const int querykeysPitchInElements,
            const int numKeys,
            Offset* const __restrict__ numValuesPerKey,
            const int numValuesPerKeyPitchInElements
        ){
            const int tid = threadIdx.x + blockDim.x * blockIdx.x;
            const int stride = blockDim.x * gridDim.x;

            constexpr int tilesize = DeviceTableView::cg_size();

            assert(stride % tilesize == 0);

            auto tile = cg::tiled_partition<tilesize>(cg::this_thread_block());
            const int tileId = tid / tilesize;
            const int numTiles = stride / tilesize;

            for(int tableid = blockIdx.y; tableid < numTables; tableid += gridDim.y){
                const DeviceTableView table = tables[tableid];
                const Key* const myQueryKeys = querykeys + querykeysPitchInElements * tableid;
                Offset* const myNumValuesPerKey = numValuesPerKey + numValuesPerKeyPitchInElements * tableid;

                for(int k = tileId; k < numKeys; k += numTiles){
                    const Key key = myQueryKeys[k];

                    const int num = table.numValues(tile, key);
                    if(tile.thread_rank() == 0){
                        myNumValuesPerKey[k] = num > maxValuesPerKey ? 0 : num;
                        //myNumValuesPerKey[k] = num;
                    }    
                }
            }
        }


    }



    template<class Key, class Value>
    class GpuHashtable{
    public:
        using MultiValueHashTable =  warpcore::MultiValueHashTable<
                Key,
                Value,
                warpcore::defaults::empty_key<Key>(),
                warpcore::defaults::tombstone_key<Key>(),
                warpcore::defaults::probing_scheme_t<Key, 8>,
                warpcore::defaults::table_storage_t<Key, Value>,
                warpcore::defaults::temp_memory_bytes()>;
        // using MultiValueHashTable =  warpcore::MultiBucketHashTable<
        //         Key,
        //         Value,
        //         warpcore::defaults::empty_key<Key>(),
        //         warpcore::defaults::tombstone_key<Key>(),
        //         warpcore::defaults::empty_key<Value>(),
        //         warpcore::defaults::probing_scheme_t<Key, 8>,
        //         warpcore::defaults::table_storage_t<Key, warpcore::ArrayBucket<Value,2>>,
        //         warpcore::defaults::temp_memory_bytes()>;

        using StatusHandler = warpcore::status_handlers::ReturnStatus;

        using Index = typename MultiValueHashTable::index_type;

        //TODO this will break if there are more than int_max keys
        using CompactKeyIndexTable = warpcore::SingleValueHashTable<
                Key,
                int,
                warpcore::defaults::empty_key<Key>(),
                warpcore::defaults::tombstone_key<Key>(),
                warpcore::defaults::probing_scheme_t<Key, 8>,
                warpcore::defaults::table_storage_t<Key, int>,
                warpcore::defaults::temp_memory_bytes()>;

        struct DeviceTableInsertView{
            MultiValueHashTable core;

            __host__ __device__
            DeviceTableInsertView(MultiValueHashTable& core_) : core(core_){}
            __host__ __device__
            DeviceTableInsertView(const DeviceTableInsertView& rhs) : core(rhs.core){}
            __host__ __device__
            DeviceTableInsertView(const DeviceTableInsertView&& rhs) : core(std::move(rhs.core)){}
            __host__ __device__
            DeviceTableInsertView& operator=(DeviceTableInsertView& rhs){
                MultiValueHashTable (&core) (rhs.core);
                return *this;
            }

            __device__
            void insert(
                Key key_in,
                const Value& value_in,
                const cg::thread_block_tile<MultiValueHashTable::cg_size()>& group
            ) noexcept{
                constexpr auto probing_length = warpcore::defaults::probing_length();

                const auto status =
                    core.insert(key_in, value_in, group, probing_length);

                //ignore status
                (void)status;
                // if(status != warpcore::Status::none() && status != warpcore::Status::max_values_for_key_reached()){
                //     printf("status error\n");
                // }

                // if(!status.has_all(warpcore::Status::duplicate_key() +
                //                      warpcore::Status::max_values_for_key_reached())){
                //     // if(status.has_any_errors()){
                //     //     printf("has_any_errors\n");
                //     // }
                //     // if(status.has_any_warnings()){
                //     //     printf("has_any_warnings\n");
                //     // }

                //     if(status.has_unknown_error()){
                //         printf("has_unknown_error\n");
                //     }
                //     if(status.has_probing_length_exceeded()){
                //         printf("has_probing_length_exceeded\n");
                //     }
                //     if(status.has_invalid_configuration()){
                //         printf("has_invalid_configuration\n");
                //     }
                //     if(status.has_invalid_key()){
                //         printf("has_invalid_key\n");
                //     }                    
                //     if(status.has_key_not_found()){
                //         printf("has_key_not_found\n");
                //     }
                //     if(status.has_index_overflow()){
                //         printf("has_index_overflow\n");
                //     }
                //     if(status.has_out_of_memory()){
                //         printf("has_out_of_memory\n");
                //     }
                //     if(status.has_not_initialized()){
                //         printf("has_not_initialized\n");
                //     }
                //     if(status.has_dry_run()){
                //         printf("has_dry_run\n");
                //     }
                //     if(status.has_invalid_phase_overlap()){
                //         printf("has_invalid_phase_overlap\n");
                //     }
                    
                //     if(status.has_invalid_value()){
                //         printf("has_invalid_value\n");
                //     }
                // }
            }

            __host__ __device__
            static constexpr int cg_size() noexcept{
                return MultiValueHashTable::cg_size();
            }
        };

        //TODO this will break if there are more than int_max keys
        struct DeviceTableView{
            CompactKeyIndexTable core;
            const int* offsets;
            const Value* values;

            DeviceTableView(const DeviceTableView&) = default;
            DeviceTableView(DeviceTableView&&) = default;

            DeviceTableView& operator=(DeviceTableView rhs){
                std::swap(*this, rhs);
                return *this;
            }


            DEVICEQUALIFIER
            int retrieve(
                cg::thread_block_tile<CompactKeyIndexTable::cg_size()> group,
                Key key,
                Value* outValues
            ) const noexcept{

                int keyIndex = 0;
                auto status = core.retrieve(
                    key,
                    keyIndex,
                    group
                );
                
                const int begin = offsets[keyIndex];
                const int end = offsets[keyIndex+1];
                const int num = end - begin;

                for(int p = group.thread_rank(); p < num; p += group.size()){
                    outValues[p] = values[begin + p];
                }

                return num;
            }

            DEVICEQUALIFIER
            int numValues(
                cg::thread_block_tile<CompactKeyIndexTable::cg_size()> group,
                Key key
            ) const noexcept{

                int keyIndex = 0;
                auto status = core.retrieve(
                    key,
                    keyIndex,
                    group
                );
                
                const int begin = offsets[keyIndex];
                const int end = offsets[keyIndex+1];
                const int num = end - begin;

                return num;
            }
        
            HOSTDEVICEQUALIFIER
            static constexpr int cg_size() noexcept{
                return CompactKeyIndexTable::cg_size();
            }
        };

        DeviceTableView makeDeviceView() const noexcept{
            return DeviceTableView{*gpuKeyIndexTable, d_compactOffsets.data(), d_compactValues.data()};
        }

        DeviceTableInsertView makeDeviceInsertView() const noexcept{
            return DeviceTableInsertView{*gpuMvTable};
        }

        

        // constexpr Key empty_key() noexcept{
        //     return MultiValueHashTable::empty_key();
        // }

        // constexpr Key tombstone_key() noexcept{
        //     return MultiValueHashTable::tombstone_key();
        // }

        GpuHashtable(){}

        GpuHashtable(std::size_t pairs_, float load_, std::size_t maxValuesPerKey_, cudaStream_t /*stream*/)
            : maxPairs(pairs_), load(load_), maxValuesPerKey(maxValuesPerKey_){

            if(maxPairs > std::size_t(std::numeric_limits<int>::max())){
                assert(maxPairs <= std::size_t(std::numeric_limits<int>::max())); //CompactKeyIndexTable uses int
            }

            cudaGetDevice(&deviceId);

            const std::size_t capacity = maxPairs / load;
            gpuMvTable = std::move(
                std::make_unique<MultiValueHashTable>(
                    capacity, warpcore::defaults::seed<Key>(), maxValuesPerKey + 1
                )
            );

        }

        HOSTDEVICEQUALIFIER
        static constexpr bool isValidKey(Key key){
            return MultiValueHashTable::is_valid_key(key);
        }

        warpcore::Status pop_status(cudaStream_t stream){
            if(isCompact){
                return gpuKeyIndexTable->pop_status(stream);
            }else{
                return gpuMvTable->pop_status(stream);
            }
        }


        void insert(
            const Key* d_keys, 
            const Value* d_values, 
            Index N, 
            cudaStream_t stream, 
            StatusHandler::base_type* d_statusarray = nullptr
        ){
            if(N == 0) return;
            assert(!isCompact);


            assert(d_keys != nullptr);
            assert(d_values != nullptr);
            assert(numKeys + N <= maxPairs);    
            assert(numValues + N <= maxPairs);

            gpuMvTable->insert(
                d_keys,
                d_values,
                N,
                stream,
                warpcore::defaults::probing_length(),
                d_statusarray
            );

            numKeys += N;
            numValues += N;
        }

//DEBUGGING
        void retrieve(
            const Key* d_keys, 
            Index N,
            Index* d_begin_offsets_out,
            Index* d_end_offsets_out,
            Value* d_values,
            cudaStream_t stream,
            StatusHandler::base_type* d_statusarray = nullptr
        ) const {
            Index num_out = 0; //TODO pinned memory?

            retrieve(
                d_keys,
                N,
                d_begin_offsets_out,
                d_end_offsets_out,
                d_values,
                num_out,
                stream,
                warpcore::defaults::probing_length(),
                d_statusarray
            );
        }
//DEBUGGING
        void retrieve(
            const Key* d_keys, 
            Index N,
            Index* d_begin_offsets_out,
            Index* d_end_offsets_out,
            Value* d_values,
            Index& num_out,
            cudaStream_t stream,
            StatusHandler::base_type* d_statusarray = nullptr
        ) const {
            gpuMvTable->retrieve(
                d_keys,
                N,
                d_begin_offsets_out,
                d_end_offsets_out,
                d_values,
                num_out,
                stream,
                warpcore::defaults::probing_length(),
                d_statusarray
            );
        }

        

        
        // template<class Offset>
        // void retrieveCompact(
        //     const Key* d_keys, 
        //     Index N,
        //     Offset* d_numValuesPerKey,
        //     Value* d_values,
        //     cudaStream_t stream,
        //     int valueOffset
        // ) const {
        //     assert(isCompact);

        //     DeviceTableView table = makeDeviceView();

        //     gpuhashtablekernels::retrieveCompactKernel<<<1024, 256, 0, stream>>>(
        //         table,
        //         d_keys,
        //         N,
        //         d_values,
        //         valueOffset, // values for key i begin at valueOffset * i
        //         d_numValuesPerKey
        //     );
        // }

        template<class Offset>
        void retrieveCompact(
            const Key* d_keys, 
            const Offset* d_beginOffsets,
            const Offset* d_numValuesPerKey,
            Index N,
            Value* d_values,
            cudaStream_t stream
        ) const {
            assert(isCompact);

            DeviceTableView table = makeDeviceView();

            gpuhashtablekernels::retrieveCompactKernel<<<1024, 256, 0, stream>>>(
                table,
                d_keys,
                d_beginOffsets,
                d_numValuesPerKey,
                maxValuesPerKey,
                N,
                d_values
            );
        }


        template<class Offset>
        void numValuesPerKeyCompact(
            const Key* d_keys, 
            Index N,
            Offset* d_numValuesPerKey,
            cudaStream_t stream
        ) const {
            assert(isCompact);

            DeviceTableView table = makeDeviceView();

            gpuhashtablekernels::numValuesPerKeyCompactKernel<<<1024, 256, 0, stream>>>(
                table,
                maxValuesPerKey,
                d_keys,
                N,
                d_numValuesPerKey
            );
        }

        MemoryUsage getMemoryInfo() const{
            const std::size_t capacity = maxPairs / load;

            MemoryUsage result;
            if(!isCompact){
                //TODO: Get correct numbers directly from table
                result.device[deviceId] = sizeof(Key) * capacity;
                result.device[deviceId] += sizeof(Value) * capacity;
            }else{
                result.device[deviceId] = (sizeof(Key) + sizeof(int)) * warpcore::detail::get_valid_capacity((numKeys / load), CompactKeyIndexTable::cg_size()); //singlevalue hashtable
                result.device[deviceId] += sizeof(int) * numKeys; //offsets
                result.device[deviceId] += sizeof(Value) * numValues; //values
            }

            return result;
        }

        HOSTDEVICEQUALIFIER INLINEQUALIFIER
        static constexpr bool is_valid_key(const Key key) noexcept{
            return MultiValueHashTable::is_valid_key(key);
        }

        std::size_t getMaxNumPairs() const noexcept{
            return maxPairs;
        }

        std::size_t getNumKeys() const noexcept{
            return numKeys;
        }

        std::size_t getNumValues() const noexcept{
            return numValues;
        }

        std::size_t getNumUniqueKeys(cudaStream_t stream = 0) const noexcept{
            return gpuMvTable->num_keys(stream);
        }

        void compact(
            void* d_temp,
            std::size_t& temp_bytes,
            cudaStream_t stream = 0
        ){
            Index numUniqueKeys = gpuMvTable != nullptr ? gpuMvTable->num_keys(stream) : 0;
            Index numValuesInTable = gpuMvTable != nullptr ? gpuMvTable->num_values(stream) : 0;

            const std::size_t batchsize = 100000;
            const std::size_t iters =  SDIV(numUniqueKeys, batchsize);

            size_t cubbytes1 = 0;
            size_t cubTempStorageBytes = 0;

            cub::DeviceReduce::Sum(
                nullptr,
                cubbytes1,
                (bool*)nullptr, 
                (int*)nullptr, 
                numValuesInTable,
                stream
            );
            cubTempStorageBytes = std::max(cubTempStorageBytes, cubbytes1);

            cub::DeviceSelect::Flagged(
                nullptr,
                cubbytes1,
                (Value*)nullptr,
                (bool*)nullptr, 
                (Value*)nullptr,
                thrust::make_discard_iterator(),
                numValuesInTable,
                stream
            );	
            cubTempStorageBytes = std::max(cubTempStorageBytes, cubbytes1);

            cub::DeviceScan::InclusiveSum(
                nullptr,
                cubbytes1,
                (int*)nullptr, 
                (int*)nullptr, 
                numUniqueKeys, 
                stream
            );
            cubTempStorageBytes = std::max(cubTempStorageBytes, cubbytes1);

            void* temp_allocations[6];
            std::size_t temp_allocation_sizes[6];
            
            temp_allocation_sizes[0] = sizeof(Key) * numUniqueKeys; // h_uniqueKeys
            temp_allocation_sizes[1] = sizeof(Index) * (numUniqueKeys+1); // h_compactOffsetTmp
            temp_allocation_sizes[2] = sizeof(Value) * numValuesInTable; // h_compactValues
            temp_allocation_sizes[3] = sizeof(bool) * numValuesInTable; // value flags
            temp_allocation_sizes[4] = sizeof(int); // reduction sum
            temp_allocation_sizes[5] = cubTempStorageBytes;            
            
            std::size_t requiredbytes = d_temp == nullptr ? 0 : temp_bytes;
            cudaError_t cubstatus = cub::AliasTemporaries(
                d_temp,
                requiredbytes,
                temp_allocations,
                temp_allocation_sizes
            );
            assert(cubstatus == cudaSuccess);

            if(d_temp == nullptr){
                temp_bytes = requiredbytes;
                return;
            }

            if(isCompact) return;

            assert(temp_bytes >= requiredbytes);

            Key* const d_tmp_uniqueKeys = static_cast<Key*>(temp_allocations[0]);
            Index* const d_tmp_compactOffset = static_cast<Index*>(temp_allocations[1]);
            Value* const d_tmp_compactValues = static_cast<Value*>(temp_allocations[2]);
            bool* const d_tmp_valueflags = static_cast<bool*>(temp_allocations[3]);
            int* const d_reductionsum = static_cast<int*>(temp_allocations[4]);
            void* const d_cubTempStorage = static_cast<void*>(temp_allocations[5]);

            Index numUniqueKeys2 = 0;
   
            gpuMvTable->retrieve_all_keys(
                d_tmp_uniqueKeys,
                numUniqueKeys2,
                stream
            ); CUDACHECKASYNC;

            if(numUniqueKeys != numUniqueKeys2){
                throw std::runtime_error("bug during hashtable construction. Expected " + std::to_string(numUniqueKeys) 
                    + " keys, got " + std::to_string(numUniqueKeys2));
            }

            retrieve(
                d_tmp_uniqueKeys,
                numUniqueKeys,
                d_tmp_compactOffset,
                d_tmp_compactOffset + 1,
                d_tmp_compactValues,
                numValuesInTable,
                stream
            );

            //clear table
            gpuMvTable.reset();
            numKeys = 0;
            numValues = 0;

            //construct new table
            gpuKeyIndexTable = std::move(
                std::make_unique<CompactKeyIndexTable>(
                    numUniqueKeys / load
                )
            );

            d_compactOffsets.resize(numUniqueKeys + 1);

            auto maxValuesPerKeytmp = maxValuesPerKey;
            
            helpers::call_fill_kernel_async(d_tmp_valueflags, numValuesInTable, true, stream);

            //disable large buckets, and buckets of size 1
            helpers::lambda_kernel<<<4096,256, 0, stream>>>(
                [
                    d_tmp_compactOffset,
                    d_compactOffsets = d_compactOffsets.data(),
                    d_tmp_valueflags,
                    numUniqueKeys,
                    maxValuesPerKeytmp
                ]__device__(){

                    for(int key = blockIdx.x; key < numUniqueKeys; key += gridDim.x){
                        const auto offsetBegin = d_tmp_compactOffset[key];
                        const auto offsetEnd = d_tmp_compactOffset[key + 1];
                        const int numValuesForKey = offsetEnd - offsetBegin;

                        if(numValuesForKey > maxValuesPerKeytmp || numValuesForKey < 2){
                            for(int i = threadIdx.x; i < numValuesForKey; i += blockDim.x){
                                d_tmp_valueflags[offsetBegin + i] = false;
                            }
                            if(threadIdx.x == 0){
                                d_compactOffsets[key+1] = 0;
                            }
                        }else{
                            if(threadIdx.x == 0){
                                d_compactOffsets[key+1] = numValuesForKey;
                            }
                        }
                    }
                }
            );
            CUDACHECKASYNC;

            CUDACHECK(cub::DeviceReduce::Sum(
                d_cubTempStorage,
                cubTempStorageBytes,
                d_tmp_valueflags, 
                d_reductionsum, 
                numValuesInTable,
                stream
            ));

            int numRemainingValuesInTable = 0;
            CUDACHECK(cudaMemcpyAsync(&numRemainingValuesInTable, d_reductionsum, sizeof(int), D2H, stream));
            CUDACHECK(cudaStreamSynchronize(stream));

            d_compactValues.resize(numRemainingValuesInTable);
            

            CUDACHECK(cub::DeviceSelect::Flagged(
                d_cubTempStorage,
                cubTempStorageBytes,
                d_tmp_compactValues,
                d_tmp_valueflags,
                d_compactValues.data(),
                thrust::make_discard_iterator(),
                numValuesInTable,
                stream
            ));

            CUDACHECK(cub::DeviceScan::InclusiveSum(
                d_cubTempStorage,
                cubTempStorageBytes,
                d_compactOffsets.data() + 1, 
                d_compactOffsets.data() + 1, 
                numUniqueKeys, 
                stream
            ));

            CUDACHECK(cudaMemsetAsync(d_compactOffsets.data(), 0, sizeof(int), stream));

            gpuhashtablekernels::callInsertIotaValuesKernel(
                *gpuKeyIndexTable,
                d_tmp_uniqueKeys,
                0,
                numUniqueKeys,
                stream
            );

            numKeys = numUniqueKeys;
            numValues = numRemainingValuesInTable;
            
            isCompact = true;

            CUDACHECK(cudaStreamSynchronize(stream));
        }

        std::size_t getMakeCopyTempBytes() const{
            if(!isCompact){
                throw std::runtime_error("Cannot compute temp bytes to create a copy of non-compact GpuHashtable");
            }
            std::size_t numPairs = gpuKeyIndexTable->size(cudaStreamPerThread);
            std::size_t keybytes = numPairs * sizeof(Key);
            std::size_t indexbytes = numPairs * sizeof(int);
            constexpr std::size_t alignment = 128;
            return SDIV(keybytes, alignment) * alignment + SDIV(indexbytes, alignment) * alignment;
        }

        //d_temp must be device-accessible on target device id
        std::unique_ptr<GpuHashtable> makeCopy(void* d_temp, int targetDeviceId) const{
            if(!isCompact){
                throw std::runtime_error("Cannot create a copy of non-compact GpuHashtable");
            }

            cub::SwitchDevice ds(targetDeviceId);

            auto result = std::make_unique<GpuHashtable>();
            result->isCompact = isCompact;
            result->deviceId = targetDeviceId;
            result->load = load;
            result->numKeys = numKeys;
            result->numValues = numValues;
            result->maxPairs = maxPairs;
            result->maxValuesPerKey = maxValuesPerKey;
            result->gpuMvTable = nullptr;


            result->d_compactOffsets.resize(d_compactOffsets.size());
            CUDACHECK(cudaMemcpyAsync(result->d_compactOffsets.data(), d_compactOffsets.data(), sizeof(int) * d_compactOffsets.size(), D2D, cudaStreamPerThread));

            result->d_compactValues.resize(d_compactValues.size());
            CUDACHECK(cudaMemcpyAsync(result->d_compactValues.data(), d_compactValues.data(), sizeof(Value) * d_compactValues.size(), D2D, cudaStreamPerThread));

            std::size_t capacity = gpuKeyIndexTable->capacity();

            result->gpuKeyIndexTable = std::make_unique<CompactKeyIndexTable>(
                capacity
            );

            auto status = result->gpuKeyIndexTable->pop_status(cudaStreamPerThread);
            CUDACHECK(cudaStreamSynchronize(cudaStreamPerThread));

            if(status.has_any_errors()){
                std::cerr << "Error creating copy of GpuHashtable. " << status << "\n";
                return nullptr;
            }

            std::size_t numPairs = gpuKeyIndexTable->size(cudaStreamPerThread);
            std::size_t keybytes = numPairs * sizeof(Key);
            std::size_t indexbytes = numPairs * sizeof(int);
            constexpr std::size_t alignment = 128;
            keybytes = SDIV(keybytes, alignment) * alignment;
            indexbytes = SDIV(indexbytes, alignment) * alignment;
            
            Key* const d_keys = reinterpret_cast<Key*>(d_temp);
            int* const d_index = reinterpret_cast<int*>(reinterpret_cast<char*>(d_keys) + keybytes);

            std::size_t numRetrieved = 0;
            //peer access
            gpuKeyIndexTable->retrieve_all(
                d_keys,
                d_index,
                numRetrieved,
                cudaStreamPerThread
            );
            CUDACHECK(cudaStreamSynchronize(cudaStreamPerThread));
            assert(numRetrieved == numPairs);

            result->gpuKeyIndexTable->insert(
                d_keys,
                d_index,
                numRetrieved,
                cudaStreamPerThread
            );

            result->gpuKeyIndexTable->pop_status(cudaStreamPerThread);
            CUDACHECK(cudaStreamSynchronize(cudaStreamPerThread));

            if(status.has_any_errors()){
                std::cerr << "Error creating copy of GpuHashtable. " << status << "\n";
                return nullptr;
            }

            return result;            
        }

        bool isCompact = false;
        int deviceId{};
        float load{};
        std::size_t numKeys{};
        std::size_t numValues{};
        std::size_t maxPairs{};
        std::size_t maxValuesPerKey{};
        CudaEvent event{};
        std::unique_ptr<MultiValueHashTable> gpuMvTable;
        std::unique_ptr<CompactKeyIndexTable> gpuKeyIndexTable;
        helpers::SimpleAllocationDevice<int, 0> d_compactOffsets;
        helpers::SimpleAllocationDevice<Value, 0> d_compactValues;
    };

    template<class Key, class Value>
    struct GpuHashtableKeyCheck{
        __host__ __device__
        bool operator()(Key key) const{
            return GpuHashtable<Key, Value>::isValidKey(key);
        }
    };

    template<class Key, class Value>
    void fixKeysForGpuHashTable(
        Key* d_keys,
        int numKeys,
        cudaStream_t stream
    ){
        dim3 block(128);
        dim3 grid(SDIV(numKeys, block.x));

        GpuHashtableKeyCheck<Key, Value> isValidKey;

        gpuhashtablekernels::fixTableKeysKernel<<<grid, block, 0, stream>>>(d_keys, numKeys, isValidKey); CUDACHECKASYNC;
    }


    #if 0

    template<class Key, class Value>
    class CpuReadOnlyMultiValueHashTableWithWarpcoreLookup{
        static_assert(std::is_integral<Key>::value, "Key must be integral!");
    public:

        using WarpcoreLookup = warpcore::SingleValueHashTable<Key, std::pair<Value, BucketSize>>;

        struct QueryResult{
            int numValues;
            const Value* valuesBegin;
        };

        CpuReadOnlyMultiValueHashTableWithWarpcoreLookup() = default;
        CpuReadOnlyMultiValueHashTableWithWarpcoreLookup(const CpuReadOnlyMultiValueHashTableWithWarpcoreLookup&) = default;
        CpuReadOnlyMultiValueHashTableWithWarpcoreLookup(CpuReadOnlyMultiValueHashTableWithWarpcoreLookup&&) = default;
        CpuReadOnlyMultiValueHashTableWithWarpcoreLookup& operator=(const CpuReadOnlyMultiValueHashTableWithWarpcoreLookup&) = default;
        CpuReadOnlyMultiValueHashTableWithWarpcoreLookup& operator=(CpuReadOnlyMultiValueHashTableWithWarpcoreLookup&&) = default;

        CpuReadOnlyMultiValueHashTableWithWarpcoreLookup(
            std::vector<Key> keys, 
            std::vector<Value> vals, 
            int maxValuesPerKey,
            const std::vector<int>& gpuIds,
            bool valuesOfSameKeyMustBeSorted = false
        ){
            init(std::move(keys), std::move(vals), maxValuesPerKey, gpuIds, valuesOfSameKeyMustBeSorted);
        }

        CpuReadOnlyMultiValueHashTableWithWarpcoreLookup(
            std::vector<Key> keys, 
            std::vector<Value> vals, 
            int maxValuesPerKey,
            bool valuesOfSameKeyMustBeSorted = false
        ){
            init(std::move(keys), std::move(vals), maxValuesPerKey, valuesOfSameKeyMustBeSorted);
        }

        CpuReadOnlyMultiValueHashTableWithWarpcoreLookup(
            std::uint64_t maxNumValues_
        ) : buildMaxNumValues{maxNumValues_}{
            buildkeys.reserve(buildMaxNumValues);
            buildvalues.reserve(buildMaxNumValues);
        }

        bool operator==(const CpuReadOnlyMultiValueHashTableWithWarpcoreLookup& rhs) const{
            return values == rhs.values && lookup == rhs.lookup;
        }

        bool operator!=(const CpuReadOnlyMultiValueHashTableWithWarpcoreLookup& rhs) const{
            return !(operator==(rhs));
        }

        void init(
            std::vector<Key> keys, 
            std::vector<Value> vals, 
            int maxValuesPerKey,
            ThreadPool* threadPool,
            bool valuesOfSameKeyMustBeSorted = false
        ){
            init(std::move(keys), std::move(vals), maxValuesPerKey, threadPool, {}, valuesOfSameKeyMustBeSorted);
        }

        void init(
            std::vector<Key> keys, 
            std::vector<Value> vals, 
            int maxValuesPerKey,
            ThreadPool* threadPool,
            const std::vector<int>& gpuIds,
            bool valuesOfSameKeyMustBeSorted = false
        ){
            assert(keys.size() == vals.size());

            //std::cerr << "init valuesOfSameKeyMustBeSorted = " << valuesOfSameKeyMustBeSorted << "\n";

            if(isInit) return;

            std::vector<read_number> countsPrefixSum;
            values = std::move(vals);

            if(keys.size() == 0) return;

            #ifdef __NVCC__            
            if(gpuIds.size() == 0){
            #endif
                using GroupByKeyCpu = care::cpu::cpuhashtabledetail::GroupByKeyCpu<Key, Value, read_number>;

                GroupByKeyCpu groupByKey(valuesOfSameKeyMustBeSorted, maxValuesPerKey);
                groupByKey.execute(keys, values, countsPrefixSum);
            #ifdef __NVCC__
            }else{

                bool success = false;

                using GroupByKeyCpu = care::cpu::cpuhashtabledetail::GroupByKeyCpu<Key, Value, read_number>;
                using GroupByKeyGpu = care::cpu::cpuhashtabledetail::GroupByKeyGpu<Key, Value, read_number>;

                #ifdef CARE_HAS_WARPCORE
                using GroupByKeyGpuWarpcore = cpuhashtabledetail::GroupByKeyGpuWarpcore<Key, Value, read_number>;

                if(true || valuesOfSameKeyMustBeSorted){

                    GroupByKeyGpu groupByKeyGpu(valuesOfSameKeyMustBeSorted, maxValuesPerKey);
                    success = groupByKeyGpu.execute(keys, values, countsPrefixSum);

                }else{

                    GroupByKeyGpuWarpcore groupByKeyGpuWarpcore(maxValuesPerKey);
                    success = groupByKeyGpuWarpcore.execute(keys, values, countsPrefixSum);

                    if(!success){
                        GroupByKeyGpu groupByKeyGpu(valuesOfSameKeyMustBeSorted, maxValuesPerKey);
                        success = groupByKeyGpu.execute(keys, values, countsPrefixSum);
                    }

                }
                #else 
                    GroupByKeyGpu groupByKeyGpu(valuesOfSameKeyMustBeSorted, maxValuesPerKey);
                    success = groupByKeyGpu.execute(keys, values, countsPrefixSum);
                #endif           

                if(!success){
                    GroupByKeyCpu groupByKeyCpu(valuesOfSameKeyMustBeSorted, maxValuesPerKey);
                    groupByKeyCpu.execute(keys, values, countsPrefixSum);
                }
            }
            #endif

            lookup = std::make_unique<WarpcoreLookup>(keys.size() / 0.8f);

            auto buildKeyLookup = [me=this, keys = std::move(keys), countsPrefixSum = std::move(countsPrefixSum)](){

                constexpr int batchsize = 500000;
                const int iterations = SDIV(keys.size(), batchsize);

                helpers::SimpleAllocationDevice<Key> d_keys(batchsize);
                helpers::SimpleAllocationDevice<read_number> d_prefixsum(batchsize+1);
                helpers::SimpleAllocationDevice<std::pair<Value, BucketSize>> d_lookupvalues(batchsize);

                for(int iter = 0; iter < iterations; iter++){
                    const int begin = iter * batchsize;
                    const int end = std::min((iter+1) * batchsize, int(keys.size()));
                    const int num = end - begin;

                    CUDACHECK(cudaMemcpyAsync(
                        d_keys.data(),
                        keys.data() + begin,
                        sizeof(Key) * num;
                        H2D,
                        cudaStreamPerThread
                    ));

                    CUDACHECK(cudaMemcpyAsync(
                        d_prefixsum.data(),
                        countsPrefixSum.data() + begin,
                        sizeof(read_number) * (num + 1);
                        H2D,
                        cudaStreamPerThread
                    ));

                    helpers::lambda_kernel<<<SDIV(batchsize, 256), 256, 0, cudaStreamPerThread>>>(
                        [
                            d_prefixsum = d_prefixsum.data(),
                            d_lookupvalues = d_lookupvalues.data(),
                            num
                        ] __device__ (){
                            const int tid = threadIdx.x + blockIdx.x * blockDim.x;

                            if(tid < num){
                                d_lookupvalues[tid].first = d_prefixsum[tid];
                                d_lookupvalues[tid].second = d_prefixsum[tid + 1] - d_prefixsum[tid];
                            }
                        }
                    ); CUDACHECKASYNC;

                    me->lookup->insert(
                        d_keys.data(),
                        d_prefixsum.data(),
                        num,
                        cudaStreamPerThread
                    );

                }

                me->isInit = true;
            };

            if(threadPool != nullptr){
                threadPool->enqueue(std::move(buildKeyLookup));
            }else{
                buildKeyLookup();
            }

        }

        void insert(const Key* keys, const Value* values, int N){
            assert(keys != nullptr);
            assert(values != nullptr);
            assert(buildMaxNumValues >= buildkeys.size() + N);

            buildkeys.insert(buildkeys.end(), keys, keys + N);
            buildvalues.insert(buildvalues.end(), values, values + N);
        }

        void finalize(int maxValuesPerKey, ThreadPool* threadPool, bool valuesOfSameKeyMustBeSorted, const std::vector<int>& gpuIds = {}){
            init(std::move(buildkeys), std::move(buildvalues), maxValuesPerKey, threadPool, gpuIds, valuesOfSameKeyMustBeSorted);            
        }

        bool isInitialized() const noexcept{
            return isInit;
        }

        void query(void* d_temp, size_t& d_tempbytes, void* h_temp, size_t& h_tempbytes, const Key* d_keys, std::size_t numKeys, QueryResult* resultsOutput, cudaStream_t stream) const{
            const int required_d_temp_bytes = sizeof(std::pair<Value, BucketSize>) * numKeys + sizeof(QueryResult) * numKeys;
            const int required_h_temp_bytes = sizeof(QueryResult) * numKeys;

            if(d_temp == nullptr){
                d_tempbytes = required_d_temp_bytes;
            }

            if(h_temp == nullptr){
                h_tempbytes = required_h_temp_bytes;
            }

            if(d_temp == nullptr || h_temp == nullptr){
                return;
            }

            assert(d_tempbytes >= required_d_temp_bytes);
            assert(h_tempbytes >= required_h_temp_bytes);

            std::pair<Value, BucketSize>* d_queryoutput = (std::pair<Value, BucketSize>*)d_temp;
            QueryResult* d_resultsOutput = (QueryResult*)(d_queryoutput + numKeys);
            QueryResult* h_resultsOutput = (QueryResult*)h_temp;

            CUDACHECK(cudaMemsetAsync(
                d_tempoutput,
                0, 
                required_d_temp_bytes,
                stream
            ));

            lookup->retrieve(
                d_keys,
                numKeys,
                d_queryoutput,
                stream
            );

            helpers::lambda_kernel<<<SDIV(numKeys, 256), 256, 0, stream>>>(
                [
                    d_queryoutput,
                    d_resultsOutput,
                    pointerToHostValues = values.data(),
                    numKeys
                ] __device__ (){
                    const int tid = threadIdx.x + blockIdx.x * blockDim.x;

                    if(tid < numKeys){
                        if(notempy){
                            d_resultsOutput[tid].numValues = d_queryoutput[tid].second;
                            const auto valuepos = d_queryoutput[tid].first;
                            d_resultsOutput[tid].valuesBegin = pointerToHostValues + valuepos;
                        }else{
                            d_resultsOutput[tid].numValues = 0;
                            d_resultsOutput[tid].valuesBegin = nullptr;
                        }
                    }
                }
            ); CUDACHECKASYNC;

            CUDACHECK(cudaMemcpyAsync(
                h_resultsOutput,
                d_resultsOutput,
                sizeof(QueryResult) * numKeys,
                D2H,
                stream
            ));

            CUDACHECK(cudaStreamSynchronize(stream));

            std::copy_n(h_resultsOutput, numKeys, resultsOutput);
        }

        MemoryUsage getMemoryInfo() const{
            MemoryUsage result;
            result.host = sizeof(Value) * values.capacity();
            result.host += lookup.getMemoryInfo().host;
            result.host += sizeof(Key) * buildkeys.capacity();
            result.host += sizeof(Value) * buildvalues.capacity();

            result.device = lookup.getMemoryInfo().device;

            std::cerr << lookup.getMemoryInfo().host << " " << result.host << " bytes\n";

            return result;
        }

        void writeToStream(std::ostream& os) const{
            assert(isInit);

            const std::size_t elements = values.size();
            const std::size_t bytes = sizeof(Value) * elements;
            os.write(reinterpret_cast<const char*>(&elements), sizeof(std::size_t));
            os.write(reinterpret_cast<const char*>(values.data()), bytes);

            lookup.writeToStream(os);
        }

        void loadFromStream(std::ifstream& is){
            destroy();

            std::size_t elements;
            is.read(reinterpret_cast<char*>(&elements), sizeof(std::size_t));
            values.resize(elements);
            const std::size_t bytes = sizeof(Value) * elements;
            is.read(reinterpret_cast<char*>(values.data()), bytes);

            lookup.loadFromStream(is);
            isInit = true;
        }

        void destroy(){
            std::vector<Value> tmp;
            std::swap(values, tmp);

            lookup.destroy();
            isInit = false;
        }

        static std::size_t estimateGpuMemoryRequiredForInit(std::size_t numElements){

            std::size_t mem = 0;
            mem += sizeof(Key) * numElements; //d_keys
            mem += sizeof(Value) * numElements; //d_values
            mem += sizeof(read_number) * numElements; //d_indices
            mem += std::max(sizeof(read_number), sizeof(Value)) * numElements; //d_indices_tmp for sorting d_indices or d_values_tmp for sorted values

            return mem;
        }

    private:

        using ValueIndex = std::pair<read_number, BucketSize>;
        bool isInit = false;
        std::uint64_t buildMaxNumValues = 0;
        std::vector<Key> buildkeys;
        std::vector<Value> buildvalues;
        // values with the same key are stored in contiguous memory locations
        // a single-value hashmap maps keys to the range of the corresponding values
        std::vector<Value> values; 
        std::unique_ptr<WarpcoreLookup> lookup;
    };

    #endif



} //namespace gpu
} //namespace care


#endif

#endif //#ifdef CARE_HAS_WARPCORE