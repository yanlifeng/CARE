
#include <gpu/cudaerrorcheck.cuh>
#include <gpu/gpuminhasher.cuh>
#include <gpu/gpureadstorage.cuh>
#include <gpu/readextender_gpu.hpp>
#include <gpu/readextension_gpu.hpp>

#include <alignmentorientation.hpp>
#include <concurrencyhelpers.hpp>
#include <config.hpp>
#include <cpu_alignment.hpp>
#include <extendedread.hpp>
#include <filehelpers.hpp>
#include <hpc_helpers.cuh>
#include <msa.hpp>
#include <options.hpp>
#include <rangegenerator.hpp>
#include <readextender_common.hpp>
#include <sequencehelpers.hpp>
#include <serializedobjectstorage.hpp>
#include <threadpool.hpp>
#include <util.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>




#include <omp.h>
#include <cub/cub.cuh>
#include <thrust/iterator/transform_iterator.h>




namespace care{
namespace gpu{

template<class T>
struct IsGreaterThan{
    T value;

    __host__ __device__
    IsGreaterThan(T t) : value(t){}

    template<class V>
    __host__ __device__
    bool operator()(V item) const noexcept{
        return item > value;
    }
};

template<class IdGenerator>
void initializeExtenderInput(
    IdGenerator& readIdGenerator,
    int requestedSizeOfTasks,
    const GpuReadStorage& gpuReadStorage,
    ReadStorageHandle& readStorageHandle,
    read_number* currentIds, // pinned memory
    int* currentReadLengths, //device accessible
    unsigned int* currentEncodedReads, //device accessible
    bool useQualityScores,
    char* currentQualityScores, //device accessible
    std::size_t encodedSequencePitchInInts,
    std::size_t qualityPitchInBytes,
    GpuReadExtender::TaskData& tasks,
    cudaStream_t stream,
    rmm::mr::device_memory_resource* mr
){
    nvtx::push_range("init", 2);

    const int maxNumPairs = (requestedSizeOfTasks - tasks.size()) / 4;

    int numNewReadsInBatch = 0;

    readIdGenerator.process_next_n(
        maxNumPairs * 2, 
        [&](auto begin, auto end){
            auto readIdsEnd = std::copy(begin, end, currentIds);
            numNewReadsInBatch = std::distance(currentIds, readIdsEnd);
        }
    );

    if(numNewReadsInBatch % 2 == 1){
        throw std::runtime_error("Input files not properly paired. Aborting read extension.");
    }
   
    if(numNewReadsInBatch > 0){
        
        gpuReadStorage.gatherSequences(
            readStorageHandle,
            currentEncodedReads,
            encodedSequencePitchInInts,
            makeAsyncConstBufferWrapper(currentIds),
            currentIds, //device accessible
            numNewReadsInBatch,
            stream,
            mr
        );

        gpuReadStorage.gatherSequenceLengths(
            readStorageHandle,
            currentReadLengths,
            currentIds,
            numNewReadsInBatch,
            stream
        );

        if(useQualityScores){
            gpuReadStorage.gatherQualities(
                readStorageHandle,
                currentQualityScores,
                qualityPitchInBytes,
                makeAsyncConstBufferWrapper(currentIds),
                currentIds, //device accessible
                numNewReadsInBatch,
                stream,
                mr
            );
        }
        
        const int numReadPairsInBatch = numNewReadsInBatch / 2; 

        //std::cerr << "thread " << std::this_thread::get_id() << "add tasks\n";
        tasks.addTasks(numReadPairsInBatch, currentIds, currentReadLengths, currentEncodedReads, currentQualityScores, stream);

        //gpuReadExtender->setState(GpuReadExtender::State::UpdateWorkingSet);

        //std::cerr << "Added " << (numReadPairsInBatch * 4) << " new tasks to batch\n";
    }

    nvtx::pop_range();
};


extension::SplittedExtensionOutput makeAndSplitExtensionOutput(
    GpuReadExtender::TaskData& finishedTasks, 
    GpuReadExtender::RawExtendResult& rawExtendResult, 
    const GpuReadExtender* gpuReadExtender, 
    bool isRepeatedIteration, 
    cudaStream_t stream
){

    nvtx::push_range("constructRawResults", 4);
    gpuReadExtender->constructRawResults(finishedTasks, rawExtendResult, stream);
    nvtx::pop_range();

    CUDACHECK(cudaStreamSynchronizeWrapper(stream));

    std::vector<extension::ExtendResult> extensionResults = gpuReadExtender->convertRawExtendResults(rawExtendResult);


    return splitExtensionOutput(extensionResults, isRepeatedIteration);
}




template<class Callback>
void extend_gpu_pairedend(
    const GoodAlignmentProperties& goodAlignmentProperties,
    const CorrectionOptions& correctionOptions,
    const ExtensionOptions& extensionOptions,
    const RuntimeOptions& runtimeOptions,
    const FileOptions& /*fileOptions*/,
    const MemoryOptions& /*memoryOptions*/,
    const GpuMinhasher& minhasher,
    const GpuReadStorage& gpuReadStorage,
    Callback submitReadyResults
){
 

    const std::uint64_t totalNumReadPairs = gpuReadStorage.getNumberOfReads() / 2;

    auto showProgress = [&](auto totalCount, auto seconds){
        if(runtimeOptions.showProgress){

            printf("Processed %10u of %10lu read pairs (Runtime: %03d:%02d:%02d)\r",
                    totalCount, totalNumReadPairs,
                    int(seconds / 3600),
                    int(seconds / 60) % 60,
                    int(seconds) % 60);
            std::cout.flush();
        }

        if(totalCount == totalNumReadPairs){
            std::cout << '\n';
        }
    };

    auto updateShowProgressInterval = [](auto duration){
        return duration;
    };

    ProgressThread<read_number> progressThread(totalNumReadPairs, showProgress, updateShowProgressInterval);

    cpu::QualityScoreConversion qualityConversion{};

    
    const int insertSize = extensionOptions.insertSize;
    const int insertSizeStddev = extensionOptions.insertSizeStddev;
    const int maximumSequenceLength = gpuReadStorage.getSequenceLengthUpperBound();
    const std::size_t encodedSequencePitchInInts = SequenceHelpers::getEncodedNumInts2Bit(maximumSequenceLength);
    const std::size_t decodedSequencePitchInBytes = SDIV(maximumSequenceLength, 128) * 128;
    const std::size_t qualityPitchInBytes = SDIV(maximumSequenceLength, 128) * 128;

    const std::size_t min_overlap = std::max(
        1, 
        std::max(
            goodAlignmentProperties.min_overlap, 
            int(maximumSequenceLength * goodAlignmentProperties.min_overlap_ratio)
        )
    );
    const std::size_t msa_max_column_count = (3*gpuReadStorage.getSequenceLengthUpperBound() - 2*min_overlap);
    //round up to 32 elements
    const std::size_t msaColumnPitchInElements = SDIV(msa_max_column_count, 32) * 32;

    std::mutex ompCriticalMutex;

    std::int64_t totalNumSuccess0 = 0;
    std::int64_t totalNumSuccess1 = 0;
    std::int64_t totalNumSuccess01 = 0;
    std::int64_t totalNumSuccessRead = 0;

    std::map<int, int> totalExtensionLengthsMap;

    std::map<int, int> totalMismatchesBetweenMateExtensions;

    //omp_set_num_threads(1);

    CUDACHECK(cudaSetDevice(runtimeOptions.deviceIds[0]));

    const int batchsizePairs = correctionOptions.batchsize;

    constexpr bool isPairedEnd = true;           

    assert(runtimeOptions.deviceIds.size() > 0);

    //will need at least one thread per gpu
    const int numDeviceIds = std::min(runtimeOptions.threads, int(runtimeOptions.deviceIds.size()));

    struct GpuData{
        int deviceId;
        std::unique_ptr<GpuReadExtender> gpuReadExtender;

        GpuData() = default;

        GpuData(const GpuData&) = delete;
        GpuData& operator=(const GpuData&) = delete;

        GpuData(GpuData&&) = default;
        GpuData& operator=(GpuData&&) = default;

        ~GpuData(){
            cub::SwitchDevice ds(deviceId);
            gpuReadExtender = nullptr;
        }
    };

    std::vector<GpuData> gpuDataVector;

    for(int d = 0; d < numDeviceIds; d++){
        const int deviceId = runtimeOptions.deviceIds[d];
        cub::SwitchDevice sd(deviceId);

        GpuData gpudata;
        gpudata.deviceId = deviceId;

        gpudata.gpuReadExtender = std::make_unique<GpuReadExtender>(
            encodedSequencePitchInInts,
            decodedSequencePitchInBytes,
            qualityPitchInBytes,
            msaColumnPitchInElements,
            isPairedEnd,
            gpuReadStorage, 
            correctionOptions,
            goodAlignmentProperties,
            qualityConversion,
            insertSize,
            insertSizeStddev,
            cudaStreamPerThread,
            rmm::mr::get_current_device_resource()
        );

        CUDACHECK(cudaStreamSynchronize(cudaStreamPerThread));

        gpuDataVector.push_back(std::move(gpudata));
    }




    auto extenderThreadFunc = [&](int gpuIndex, int /*threadId*/, auto* readIdGenerator, bool isRepeatedIteration, bool /*isLastIteration*/, bool extraHashing, GpuReadExtender::IterationConfig iterationConfig){
        //std::cerr << "extenderThreadFunc( " << gpuIndex << ", " << threadId << ")\n";
        auto& gpudata = gpuDataVector[gpuIndex];

        cudaStream_t stream = cudaStreamPerThread;

        CUDACHECK(cudaSetDevice(gpudata.deviceId));

        rmm::mr::device_memory_resource* rmmDeviceResource = rmm::mr::get_current_device_resource();

        std::int64_t numSuccess0 = 0;
        std::int64_t numSuccess1 = 0;
        std::int64_t numSuccess01 = 0;
        std::int64_t numSuccessRead = 0;

        std::map<int, int> extensionLengthsMap;
        std::map<int, int> mismatchesBetweenMateExtensions;

        ReadStorageHandle readStorageHandle = gpuReadStorage.makeHandle();


        helpers::SimpleAllocationPinnedHost<read_number> currentIds(2 * batchsizePairs);
        helpers::SimpleAllocationDevice<unsigned int> currentEncodedReads(2 * encodedSequencePitchInInts * batchsizePairs);
        helpers::SimpleAllocationDevice<int> currentReadLengths(2 * batchsizePairs);
        helpers::SimpleAllocationDevice<char> currentQualityScores(2 * qualityPitchInBytes * batchsizePairs);

        if(!correctionOptions.useQualityScores){
            helpers::call_fill_kernel_async(currentQualityScores.data(), currentQualityScores.size(), 'I', stream);
        }


        GpuReadExtender::Hasher anchorHasher(minhasher, rmmDeviceResource);

        GpuReadExtender::TaskData tasks(rmmDeviceResource, 0, encodedSequencePitchInInts, decodedSequencePitchInBytes, qualityPitchInBytes, stream);
        GpuReadExtender::TaskData finishedTasks(rmmDeviceResource, 0, encodedSequencePitchInInts, decodedSequencePitchInBytes, qualityPitchInBytes, stream);

        GpuReadExtender::AnchorData anchorData(rmmDeviceResource);
        GpuReadExtender::AnchorHashResult anchorHashResult(rmmDeviceResource);

        GpuReadExtender::RawExtendResult rawExtendResult{};

        std::vector<read_number> pairsWhichShouldBeRepeated;

        auto output = [&](){

            nvtx::push_range("output", 5);

            nvtx::push_range("convert extension results", 7);

            auto splittedExtOutput = makeAndSplitExtensionOutput(finishedTasks, rawExtendResult, gpudata.gpuReadExtender.get(), isRepeatedIteration, stream);

            nvtx::pop_range();

            pairsWhichShouldBeRepeated.insert(
                pairsWhichShouldBeRepeated.end(), 
                splittedExtOutput.idsOfPartiallyExtendedReads.begin(), 
                splittedExtOutput.idsOfPartiallyExtendedReads.end()
            );

            const std::size_t numExtended = splittedExtOutput.extendedReads.size();

            if(!extensionOptions.allowOutwardExtension){
                for(auto& er : splittedExtOutput.extendedReads){
                    er.removeOutwardExtension();
                }
            }

            std::vector<EncodedExtendedRead> encvec(numExtended);
            for(std::size_t i = 0; i < numExtended; i++){
                splittedExtOutput.extendedReads[i].encodeInto(encvec[i]);
            }

            submitReadyResults(
                std::move(splittedExtOutput.extendedReads), 
                std::move(encvec),
                std::move(splittedExtOutput.idsOfNotExtendedReads)
            );

            progressThread.addProgress(numExtended);

            nvtx::pop_range();
        };

        while(!(readIdGenerator->empty() && tasks.size() == 0)){
            if(int(tasks.size()) < (batchsizePairs * 4) / 2){
                initializeExtenderInput(
                    *readIdGenerator,
                    batchsizePairs * 4,
                    gpuReadStorage,
                    readStorageHandle,
                    currentIds.data(), 
                    currentReadLengths.data(), 
                    currentEncodedReads.data(),
                    correctionOptions.useQualityScores,
                    currentQualityScores.data(), 
                    encodedSequencePitchInInts,
                    qualityPitchInBytes,
                    tasks,
                    stream,
                    rmmDeviceResource
                );
            }

            tasks.aggregateAnchorData(anchorData, stream);
            
            nvtx::push_range("getCandidateReadIds", 4);
            if(extraHashing){
            //if(false){
                anchorHasher.getCandidateReadIdsWithExtraExtensionHash(
                    anchorData, 
                    anchorHashResult,
                    iterationConfig, 
                    thrust::make_transform_iterator(
                        tasks.iteration.data(),
                        IsGreaterThan<int>{0}
                    ),
                    stream
                );
            }else{
                anchorHasher.getCandidateReadIds(anchorData, anchorHashResult, stream);
            }
            // #if 0
            // anchorHasher.getCandidateReadIds(anchorData, anchorHashResult, stream);
            // #else
            // anchorHasher.getCandidateReadIdsWithExtraExtensionHash(
            //     *gpudata.dataAllocator,
            //     anchorData, 
            //     anchorHashResult,
            //     iterationConfig, 
            //     thrust::make_transform_iterator(
            //         tasks.iteration.data(),
            //         IsGreaterThan<int>{0}
            //     ),
            //     stream
            // );
            // #endif
            nvtx::pop_range();

            gpudata.gpuReadExtender->processOneIteration(
                tasks,
                anchorData, 
                anchorHashResult, 
                finishedTasks, 
                iterationConfig,
                stream
            );

            CUDACHECK(cudaStreamSynchronizeWrapper(stream));
            
            if(finishedTasks.size() > std::size_t((batchsizePairs * 4) / 2)){
                output();
            }

            //std::cerr << "Remaining: tasks " << tasks.size() << ", finishedtasks " << gpuReadExtender->finishedTasks->size() << "\n";
        }

        output();
        assert(finishedTasks.size() == 0);

        {
            std::lock_guard<std::mutex> lg(ompCriticalMutex);

            totalNumSuccess0 += numSuccess0;
            totalNumSuccess1 += numSuccess1;
            totalNumSuccess01 += numSuccess01;
            totalNumSuccessRead += numSuccessRead;

            for(const auto& pair : extensionLengthsMap){
                totalExtensionLengthsMap[pair.first] += pair.second;
            }

            for(const auto& pair : mismatchesBetweenMateExtensions){
                totalMismatchesBetweenMateExtensions[pair.first] += pair.second;
            }   
        }

        
        gpuReadStorage.destroyHandle(readStorageHandle);

        return pairsWhichShouldBeRepeated;
    };

    bool isLastIteration = false;
    GpuReadExtender::IterationConfig iterationConfig;
    iterationConfig.maxextensionPerStep = extensionOptions.fixedStepsize == 0 ? 20 : extensionOptions.fixedStepsize;
    iterationConfig.minCoverageForExtension = 3;

    std::vector<read_number> pairsWhichShouldBeRepeated;
    std::vector<read_number> pairsWhichShouldBeRepeatedTmp;

    for(auto& x : gpuDataVector){
        x.gpuReadExtender->insertSizeStddev = extensionOptions.fixedStddev == 0 ? extensionOptions.insertSizeStddev : extensionOptions.fixedStddev;
    }


    {
        std::vector<std::future<std::vector<read_number>>> futures;

        const std::size_t numReadsToProcess = 100000;
        //const std::size_t numReadsToProcess = gpuReadStorage.getNumberOfReads();

        // std::vector<read_number> idsToExtend{
        //     0, 1, 22, 23, 44, 45, 68, 69, 78, 79, 86, 87, 98, 99,
        //     112, 113, 136,137,180,181,198,199,202,203,266,267,
        //     290,291,316,317,350,351,402,403,436,437,446,447,498,499,574,575,582,583,588,589,
        //     598,599,692,693,704,705,814,815,964,965,966,967,970,971,1026,1027
        // };

        // IteratorRangeTraversal<decltype(idsToExtend.begin())> readIdGenerator(
        //     idsToExtend.begin(),
        //     idsToExtend.end()
        // );

        IteratorRangeTraversal<thrust::counting_iterator<read_number>> readIdGenerator(
            thrust::make_counting_iterator<read_number>(0),
            thrust::make_counting_iterator<read_number>(0) + numReadsToProcess
        );

        // IteratorRangeTraversal<thrust::counting_iterator<read_number>> readIdGenerator(
        //     thrust::make_counting_iterator<read_number>(0) + 9779220,
        //     thrust::make_counting_iterator<read_number>(0) + 9779224
        // );

        const int maxNumThreads = runtimeOptions.threads;
        const bool extraHashing = false;

        std::cerr << "First iteration. insertsizedev: " << gpuDataVector[0].gpuReadExtender->insertSizeStddev 
        << ", maxextensionPerStep: " << iterationConfig.maxextensionPerStep
        << ", minCoverageForExtension: " << iterationConfig.minCoverageForExtension
        << ", isLastIteration: " << isLastIteration 
        << ", extraHashing: " << extraHashing << "\n";

        std::cerr << "use " << maxNumThreads << " threads\n";

        constexpr bool isRepeatedIteration = false;

        for(int t = 0; t < maxNumThreads; t++){
            futures.emplace_back(
                std::async(
                    std::launch::async,
                    extenderThreadFunc,
                    t % numDeviceIds,
                    t,
                    &readIdGenerator,
                    isRepeatedIteration,
                    isLastIteration,
                    extraHashing,
                    iterationConfig
                )
            );
        }

        for(auto& f : futures){
            auto vec = f.get();
            pairsWhichShouldBeRepeatedTmp.insert(pairsWhichShouldBeRepeatedTmp.end(), vec.begin(), vec.end());
        }

        std::swap(pairsWhichShouldBeRepeated, pairsWhichShouldBeRepeatedTmp);
        pairsWhichShouldBeRepeatedTmp.clear();
        std::sort(pairsWhichShouldBeRepeated.begin(), pairsWhichShouldBeRepeated.end());

        //iterationConfig.maxextensionPerStep -= 4;
    }

    if(!isLastIteration){

        for(auto& x : gpuDataVector){
            x.gpuReadExtender->insertSizeStddev = extensionOptions.fixedStddev == 0 ? 40 : extensionOptions.fixedStddev;
            //x.gpuReadExtender->insertSizeStddev = extensionOptions.fixedStddev == 0 ? 40 : 40;
        }

        const bool extraHashing = true;

        isLastIteration = false;
        constexpr bool isRepeatedIteration = true;
        //iterationConfig.maxextensionPerStep = 16;

        //while(pairsWhichShouldBeRepeated.size() > 0 && (iterationConfig.maxextensionPerStep > 0))
        {
            const int numPairsToRepeat = pairsWhichShouldBeRepeated.size() / 2;
            std::cerr << "Will repeat extension of " << numPairsToRepeat << " read pairs with fixedStepsize = " << iterationConfig.maxextensionPerStep << "\n";

            std::cerr << "Second iteration. insertsizedev: " << gpuDataVector[0].gpuReadExtender->insertSizeStddev 
            << ", maxextensionPerStep: " << iterationConfig.maxextensionPerStep
            << ", minCoverageForExtension: " << iterationConfig.minCoverageForExtension
            << ", isLastIteration: " << isLastIteration 
            << ", extraHashing: " << extraHashing << "\n";

            //isLastIteration = (iterationConfig.maxextensionPerStep <= 4);

            auto readIdGenerator = makeIteratorRangeTraversal(
                pairsWhichShouldBeRepeated.data(), 
                pairsWhichShouldBeRepeated.data() + pairsWhichShouldBeRepeated.size()
            );

            const int threadsForPairs = SDIV(numPairsToRepeat, batchsizePairs);
            const int maxNumThreads = std::min(threadsForPairs, runtimeOptions.threads);
            std::cerr << "use " << maxNumThreads << " threads\n";

            std::vector<std::future<std::vector<read_number>>> futures;

            for(int t = 0; t < maxNumThreads; t++){
                futures.emplace_back(
                    std::async(
                        std::launch::async,
                        extenderThreadFunc,
                        t % numDeviceIds,
                        t,
                        &readIdGenerator,
                        isRepeatedIteration,
                        isLastIteration,
                        extraHashing,
                        iterationConfig
                    )
                );
            }

            for(auto& f : futures){
                auto vec = f.get();
                pairsWhichShouldBeRepeatedTmp.insert(pairsWhichShouldBeRepeatedTmp.end(), vec.begin(), vec.end());
            }

            std::swap(pairsWhichShouldBeRepeated, pairsWhichShouldBeRepeatedTmp);
            pairsWhichShouldBeRepeatedTmp.clear();
            std::sort(pairsWhichShouldBeRepeated.begin(), pairsWhichShouldBeRepeated.end());

            submitReadyResults(
                {}, 
                {},
                std::move(pairsWhichShouldBeRepeated) //pairs which did not find mate after repetition will remain unextended
            );

            iterationConfig.maxextensionPerStep -= 4;
        }
    }
   

    progressThread.finished();

    // std::cout << "totalNumSuccess0: " << totalNumSuccess0 << std::endl;
    // std::cout << "totalNumSuccess1: " << totalNumSuccess1 << std::endl;
    // std::cout << "totalNumSuccess01: " << totalNumSuccess01 << std::endl;
    // std::cout << "totalNumSuccessRead: " << totalNumSuccessRead << std::endl;

    // std::cout << "Extension lengths:\n";

    // for(const auto& pair : totalExtensionLengthsMap){
    //     std::cout << pair.first << ": " << pair.second << "\n";
    // }

    // std::cout << "mismatches between mate extensions:\n";

    // for(const auto& pair : totalMismatchesBetweenMateExtensions){
    //     std::cout << pair.first << ": " << pair.second << "\n";
    // }


}


#if 0

SerializedObjectStorage extend_gpu_singleend(
    const GoodAlignmentProperties& goodAlignmentProperties,
    const CorrectionOptions& correctionOptions,
    const ExtensionOptions& extensionOptions,
    const RuntimeOptions& runtimeOptions,
    const FileOptions& fileOptions,
    const MemoryOptions& memoryOptions,
    const GpuMinhasher& minhasher,
    const GpuReadStorage& gpuReadStorage
){
    std::cerr << "extend_gpu_singleend\n";
    throw std::runtime_error("extend_gpu_singleend NOT IMPLEMENTED");
    
}
#endif

void extend_gpu(
    const GoodAlignmentProperties& goodAlignmentProperties,
    const CorrectionOptions& correctionOptions,
    const ExtensionOptions& extensionOptions,
    const RuntimeOptions& runtimeOptions,
    const FileOptions& fileOptions,
    const MemoryOptions& memoryOptions,
    const GpuMinhasher& gpumMinhasher,
    const GpuReadStorage& gpuReadStorage,
    SubmitReadyExtensionResultsCallback submitReadyResults
){
    // if(fileOptions.pairType == SequencePairType::SingleEnd){
    //     return extend_gpu_singleend(
    //         goodAlignmentProperties,
    //         correctionOptions,
    //         extensionOptions,
    //         runtimeOptions,
    //         fileOptions,
    //         memoryOptions,
    //         gpumMinhasher,
    //         gpuReadStorage
    //     );
    // }else{
        extend_gpu_pairedend(
            goodAlignmentProperties,
            correctionOptions,
            extensionOptions,
            runtimeOptions,
            fileOptions,
            memoryOptions,
            gpumMinhasher,
            gpuReadStorage,
            submitReadyResults
        );
    //}
}





} // namespace gpu

} // namespace care