#ifndef READ_EXTENDER_GPU_HPP
#define READ_EXTENDER_GPU_HPP

#include <readextenderbase.hpp>
#include <config.hpp>
#include <hpc_helpers.cuh>

#include <gpu/gpumsa.cuh>
#include <gpu/kernels.hpp>
#include <gpu/kernellaunch.hpp>
#include <gpu/gpuminhasher.cuh>
#include <gpu/segmented_set_operations.cuh>
#include <gpu/cachingallocator.cuh>
#include <sequencehelpers.hpp>
#include <hostdevicefunctions.cuh>
#include <util.hpp>

#include <algorithm>
#include <vector>
#include <numeric>

#include <cub/cub.cuh>

#include <thrust/device_new_allocator.h>

#include <thrust/iterator/zip_iterator.h>
#include <thrust/iterator/transform_iterator.h>




namespace care{




namespace readextendergpukernels{

    __global__
    void flagCandidateIdsWhichAreEqualToAnchorOrMateKernel(
        const read_number* __restrict__ candidateReadIds,
        const read_number* __restrict__ anchorReadIds,
        const read_number* __restrict__ mateReadIds,
        const int* __restrict__ numCandidatesPerAnchorPrefixSum,
        const int* __restrict__ numCandidatesPerAnchor,
        bool* __restrict__ keepflags, // size numCandidates
        bool* __restrict__ mateRemovedFlags, //size numTasks
        int* __restrict__ numCandidatesPerAnchorOut,
        int numTasks,
        bool isPairedEnd
    );

    template<int dummy=0>
    __global__
    void setAnchorIndicesOfCandidateskernel(
        int* __restrict__ d_anchorIndicesOfCandidates,
        const int* __restrict__ numAnchorsPtr,
        const int* __restrict__ d_candidates_per_anchor,
        const int* __restrict__ d_candidates_per_anchor_prefixsum
    ){
        for(int anchorIndex = blockIdx.x; anchorIndex < *numAnchorsPtr; anchorIndex += gridDim.x){
            const int offset = d_candidates_per_anchor_prefixsum[anchorIndex];
            const int numCandidatesOfAnchor = d_candidates_per_anchor[anchorIndex];
            int* const beginptr = &d_anchorIndicesOfCandidates[offset];

            for(int localindex = threadIdx.x; localindex < numCandidatesOfAnchor; localindex += blockDim.x){
                beginptr[localindex] = anchorIndex;
            }
        }
    }
    

    template<int blocksize>
    __global__
    void reverseComplement2bitKernel(
        const int* __restrict__ lengths,
        const unsigned int* __restrict__ forward,
        unsigned int* __restrict__ reverse,
        int num,
        int encodedSequencePitchInInts
    ){

        for(int s = threadIdx.x + blockIdx.x * blockDim.x; s < num; s += blockDim.x * gridDim.x){
            const unsigned int* input = forward + encodedSequencePitchInInts * s;
            unsigned int* output = reverse + encodedSequencePitchInInts * s;
            const int length = lengths[s];

            SequenceHelpers::reverseComplementSequence2Bit(
                output,
                input,
                length,
                [](auto i){return i;},
                [](auto i){return i;}
            );
        }

        // constexpr int smemsizeints = blocksize * 16;
        // __shared__ unsigned int sharedsequences[smemsizeints]; //sequences will be stored transposed

        // const int sequencesPerSmem = std::min(blocksize, smemsizeints / encodedSequencePitchInInts);
        // assert(sequencesPerSmem > 0);

        // const int smemiterations = SDIV(num, sequencesPerSmem);

        // for(int smemiteration = blockIdx.x; smemiteration < smemiterations; smemiteration += gridDim.x){

        //     const int idBegin = smemiteration * sequencesPerSmem;
        //     const int idEnd = std::min((smemiteration+1) * sequencesPerSmem, num);

        //     __syncthreads();

        //     for(int s = idBegin + threadIdx.x; s < idEnd; s += blockDim.x){
        //         for(int intindex = 0; intindex < encodedSequencePitchInInts; intindex++){ //load intindex-th element of sequence s
        //             sharedsequences[intindex * sequencesPerSmem + s] = forward[encodedSequencePitchInInts * s + intindex];
        //         }
        //     }

        //     __syncthreads();

        //     for(int s = idBegin + threadIdx.x; s < idEnd; s += blockDim.x){
        //         SequenceHelpers::reverseComplementSequenceInplace2Bit(&sharedsequences[s], lengths[s], [&](auto i){return i * sequencesPerSmem;});
        //     }

        //     __syncthreads();

        //     for(int s = idBegin + threadIdx.x; s < idEnd; s += blockDim.x){
        //         for(int intindex = 0; intindex < encodedSequencePitchInInts; intindex++){ //load intindex-th element of sequence s
        //             reverse[encodedSequencePitchInInts * s + intindex] = sharedsequences[intindex * sequencesPerSmem + s];
        //         }
        //     }
        // }
    }


    template<int blocksize, int groupsize>
    __global__
    void filtermatekernel(
        const unsigned int* __restrict__ anchormatedata,
        const unsigned int* __restrict__ candidatefwddata,
        //const unsigned int* __restrict__ candidatefwddata2,
        int encodedSequencePitchInInts,
        const int* __restrict__ numCandidatesPerAnchor,
        const int* __restrict__ numCandidatesPerAnchorPrefixSum,
        const int* __restrict__ activeTaskIndices,
        int numTasksWithRemovedMate,
        bool* __restrict__ output_keepflags
    ){

        auto group = cg::tiled_partition<groupsize>(cg::this_thread_block());
        const int groupindex = (threadIdx.x + blockIdx.x * blockDim.x) / groupsize;
        const int numgroups = (gridDim.x * blockDim.x) / groupsize;
        const int groupindexinblock = threadIdx.x / groupsize;

        static_assert(blocksize % groupsize == 0);
        //constexpr int groupsperblock = blocksize / groupsize;

        extern __shared__ unsigned int smem[]; //sizeof(unsigned int) * groupsperblock * encodedSequencePitchInInts

        unsigned int* sharedMate = smem + groupindexinblock * encodedSequencePitchInInts;

        for(int task = groupindex; task < numTasksWithRemovedMate; task += numgroups){

            const int globalTaskIndex = activeTaskIndices[task];
            const int numCandidates = numCandidatesPerAnchor[globalTaskIndex];
            const int candidatesOffset = numCandidatesPerAnchorPrefixSum[globalTaskIndex];

            for(int p = group.thread_rank(); p < encodedSequencePitchInInts; p++){
                sharedMate[p] = anchormatedata[encodedSequencePitchInInts * task + p];
            }
            group.sync();

            //compare mate to candidates. 1 thread per candidate
            for(int c = group.thread_rank(); c < numCandidates; c += group.size()){
                bool doKeep = false;
                const unsigned int* const candidateptr = candidatefwddata + encodedSequencePitchInInts * (candidatesOffset + c);

                for(int p = 0; p < encodedSequencePitchInInts; p++){
                    const unsigned int aaa = sharedMate[p];
                    const unsigned int bbb = candidateptr[p];

                    if(aaa != bbb){
                        doKeep = true;
                        break;
                    }
                }

                output_keepflags[(candidatesOffset + c)] = doKeep;
            }
        }
    }
}


struct SequenceFlagMultiplier{
    int pitch{};
    const bool* flags{};

    __host__ __device__
    SequenceFlagMultiplier(const bool* flags_, int pitch_)
        :pitch(pitch_), flags(flags_){

    }

    __host__ __device__
    bool operator()(int i) const{
        return flags[i / pitch];
    }
};

struct BatchData{
    template<class T>
    using DeviceBuffer = helpers::SimpleAllocationDevice<T>;
    //using DeviceBuffer = helpers::SimpleAllocationPinnedHost<T>;

    template<class T>
    using PinnedBuffer = helpers::SimpleAllocationPinnedHost<T>;

    enum class State{
        BeforePrepare,
        BeforeHash,
        BeforeStep,
        BeforeExtend,
        BeforeOutput,
        None
    };

    std::string to_string(State s) const{
        switch(s){
            case State::BeforePrepare: return "BeforePrepare";
            case State::BeforeHash: return "BeforeHash";
            case State::BeforeStep: return "BeforeStep";
            case State::BeforeExtend: return "BeforeExtend";
            case State::BeforeOutput: return "BeforeOutput";
            case State::None: return "None";
            default: return "Missing case BatchData::to_string(State)\n";
        };
    }

    bool isEmpty() const noexcept{
        return indicesOfActiveTasks.empty();
    }

    void setState(State newstate){      
        #if 0
        std::cerr << "batchdata " << someId << " statechange " << to_string(state) << " -> " << to_string(newstate) << "\n";
        #endif

        state = newstate;
    }




    bool needPrepareStep = false;
    bool pairedEnd = false;
    State state = State::None;
    int numTasks = 0;
    int numTasksWithMateRemoved = 0;
    int someId = 0;
    int numReadPairs = 0;

    int totalNumCandidates = 0;
    int totalNumberOfUsedIds = 0;

    std::size_t encodedSequencePitchInInts = 0;
    std::size_t decodedSequencePitchInBytes = 0;
    std::size_t msaColumnPitchInElements = 0;
    std::size_t qualityPitchInBytes = 0;

    std::size_t outputAnchorPitchInBytes = 0;
    std::size_t outputAnchorQualityPitchInBytes = 0;
    std::size_t decodedMatesRevCPitchInBytes = 0;

    PinnedBuffer<read_number> h_anchorReadIds{};
    DeviceBuffer<read_number> d_anchorReadIds{};
    PinnedBuffer<read_number> h_mateReadIds{};
    DeviceBuffer<read_number> d_mateReadIds{};
    PinnedBuffer<read_number> h_candidateReadIds{};
    PinnedBuffer<read_number> h_candidateReadIds2{};
    DeviceBuffer<read_number> d_candidateReadIds{};

    PinnedBuffer<int> h_anchorIndicesOfCandidates{};
    PinnedBuffer<int> h_anchorIndicesOfCandidates2{};

    PinnedBuffer<bool> h_isPairedCandidate{};
    DeviceBuffer<bool> d_isPairedCandidate{};
    DeviceBuffer<bool> d_isPairedCandidate2{};

    DeviceBuffer<int> d_anchorIndicesOfCandidates{};
    DeviceBuffer<int> d_anchorIndicesOfCandidates2{};
    DeviceBuffer<int> d_segmentIdsOfUsedReadIds{};

    PinnedBuffer<int> h_segmentIdsOfUsedReadIds{};
    PinnedBuffer<int> h_segmentIdsOfReadIds{};

    DeviceBuffer<unsigned int> d_anchormatedata{};


    PinnedBuffer<unsigned int> h_inputanchormatedata{};
    DeviceBuffer<unsigned int> d_inputanchormatedata{};

    DeviceBuffer<int> d_anchorIndicesWithRemovedMates{};

    PinnedBuffer<int> h_numCandidatesPerAnchor{};
    PinnedBuffer<int> h_numCandidatesPerAnchor2{};
    DeviceBuffer<int> d_numCandidatesPerAnchor{};
    DeviceBuffer<int> d_numCandidatesPerAnchor2{};
    PinnedBuffer<int> h_numCandidatesPerAnchorPrefixSum{};
    PinnedBuffer<int> h_numCandidatesPerAnchorPrefixSum2{};
    DeviceBuffer<int> d_numCandidatesPerAnchorPrefixSum{};
    DeviceBuffer<int> d_numCandidatesPerAnchorPrefixSum2{};
    PinnedBuffer<int> h_alignment_overlaps{};
    PinnedBuffer<int> h_alignment_shifts{};
    PinnedBuffer<int> h_alignment_nOps{};

    PinnedBuffer<BestAlignment_t> h_alignment_best_alignment_flags{};

    DeviceBuffer<int> d_alignment_overlaps{};
    DeviceBuffer<int> d_alignment_shifts{};
    DeviceBuffer<int> d_alignment_nOps{};
    DeviceBuffer<BestAlignment_t> d_alignment_best_alignment_flags{};

    DeviceBuffer<int> d_alignment_overlaps2{};
    DeviceBuffer<int> d_alignment_shifts2{};
    DeviceBuffer<int> d_alignment_nOps2{};
    DeviceBuffer<BestAlignment_t> d_alignment_best_alignment_flags2{};

    PinnedBuffer<int> h_numAnchors{};
    PinnedBuffer<int> h_numCandidates{};
    DeviceBuffer<int> d_numAnchors{};
    DeviceBuffer<int> d_numCandidates{};
    DeviceBuffer<int> d_numCandidates2{};
    PinnedBuffer<int> h_numAnchorsWithRemovedMates{};

    PinnedBuffer<int> h_anchorSequencesLength{};
    DeviceBuffer<int> d_anchorSequencesLength{};
    PinnedBuffer<int> h_candidateSequencesLength{};
    DeviceBuffer<int> d_candidateSequencesLength{};
    PinnedBuffer<unsigned int> h_subjectSequencesData{};
    PinnedBuffer<char> h_subjectSequencesDataDecoded{};
    PinnedBuffer<unsigned int> h_candidateSequencesData{};
    PinnedBuffer<unsigned int> h_candidateSequencesRevcData{};

    DeviceBuffer<int> d_candidateSequencesLength2{};
    DeviceBuffer<unsigned int> d_candidateSequencesData2{};
    DeviceBuffer<unsigned int> d_candidateSequencesRevcData2{};
    DeviceBuffer<read_number> d_candidateReadIds2{};
    

    DeviceBuffer<unsigned int> d_subjectSequencesData{};
    DeviceBuffer<char> d_subjectSequencesDataDecoded{};
    DeviceBuffer<unsigned int> d_candidateSequencesData{};
    DeviceBuffer<unsigned int> d_candidateSequencesRevcData{};

    PinnedBuffer<char> h_anchorQualityScores{};
    DeviceBuffer<char> d_anchorQualityScores{};
    PinnedBuffer<char> h_candidateQualityScores{};

    DeviceBuffer<int> d_activeTaskIndices{};
    PinnedBuffer<int> h_activeTaskIndices{};

    PinnedBuffer<read_number> h_usedReadIds{};
    PinnedBuffer<int> h_numUsedReadIdsPerAnchor{};
    PinnedBuffer<int> h_numUsedReadIdsPerAnchorPrefixSum{};

    DeviceBuffer<read_number> d_usedReadIds{};
    DeviceBuffer<int> d_numUsedReadIdsPerAnchor{};
    DeviceBuffer<int> d_numUsedReadIdsPerAnchorPrefixSum{};

    PinnedBuffer<char> h_consensus;
    PinnedBuffer<gpu::MSAColumnProperties> h_msa_column_properties;

    DeviceBuffer<std::uint8_t> d_consensusEncoded; //encoded , 0-4
    DeviceBuffer<int> d_coverage;
    DeviceBuffer<gpu::MSAColumnProperties> d_msa_column_properties;

    PinnedBuffer<int> h_coverage;

    PinnedBuffer<char> h_consensusQuality;
    DeviceBuffer<char> d_consensusQuality;

    helpers::SimpleAllocationPinnedHost<int> h_inputMateLengths;
    helpers::SimpleAllocationPinnedHost<bool> h_isPairedTask;
    helpers::SimpleAllocationPinnedHost<char> h_decodedMatesRevC;
    helpers::SimpleAllocationPinnedHost<int> h_scatterMap;

    helpers::SimpleAllocationPinnedHost<int> h_accumExtensionsLengths;
    helpers::SimpleAllocationPinnedHost<AbortReason> h_abortReasons;
    helpers::SimpleAllocationPinnedHost<char> h_outputAnchors;
    helpers::SimpleAllocationPinnedHost<char> h_outputAnchorQualities;
    helpers::SimpleAllocationPinnedHost<int> h_outputAnchorLengths;
    helpers::SimpleAllocationPinnedHost<bool> h_outputMateHasBeenFound;
    helpers::SimpleAllocationPinnedHost<int> h_sizeOfGapToMate;

    
    std::array<CudaEvent, 1> events{};
    std::array<CudaStream, 4> streams{};
    std::vector<int> indicesOfActiveTasks{};
    std::vector<ReadExtenderBase::Task> tasks;

};




struct GpuReadHasher{
public:
    GpuReadHasher(
        const gpu::GpuMinhasher& mh
    ) : gpuMinhasher(&mh),
        minhashHandle(gpuMinhasher->makeQueryHandle()) {
    }

    void getCandidateReadIds(BatchData& batchData) const{

        int totalNumValues = 0;

        gpuMinhasher->determineNumValues(
            minhashHandle,
            batchData.d_subjectSequencesData.get(),
            batchData.encodedSequencePitchInInts,
            batchData.d_anchorSequencesLength.get(),
            batchData.numTasks,
            batchData.d_numCandidatesPerAnchor.get(),
            totalNumValues,
            batchData.streams[0]
        );

        cudaStreamSynchronize(batchData.streams[0]); CUERR;

        batchData.d_candidateReadIds.resize(totalNumValues);        

        if(totalNumValues == 0){
            cudaMemsetAsync(batchData.d_numCandidatesPerAnchor.get(), 0, sizeof(int) * batchData.numTasks , batchData.streams[0]); CUERR;
            cudaMemsetAsync(batchData.d_numCandidatesPerAnchorPrefixSum.get(), 0, sizeof(int) * (1 + batchData.numTasks), batchData.streams[0]); CUERR;
            batchData.totalNumCandidates = 0;
            return;
        }

        gpuMinhasher->retrieveValues(
            minhashHandle,
            nullptr,
            batchData.numTasks,              
            totalNumValues,
            batchData.d_candidateReadIds.get(),
            batchData.d_numCandidatesPerAnchor.get(),
            batchData.d_numCandidatesPerAnchorPrefixSum.get(),
            batchData.streams[0]
        );

        cudaMemcpyAsync(
            &batchData.totalNumCandidates,
            batchData.d_numCandidatesPerAnchorPrefixSum.data() + batchData.numTasks,
            sizeof(int),
            D2H,
            batchData.streams[0]
        ); CUERR;

        cudaStreamSynchronize(batchData.streams[0]); CUERR;
    }
    
    void getCandidateReadIds(BatchData& batchData, cudaStream_t stream) const{

        int totalNumValues = 0;

        gpuMinhasher->determineNumValues(
            minhashHandle,
            batchData.d_subjectSequencesData.get(),
            batchData.encodedSequencePitchInInts,
            batchData.d_anchorSequencesLength.get(),
            batchData.numTasks,
            batchData.d_numCandidatesPerAnchor.get(),
            totalNumValues,
            stream
        );

        cudaStreamSynchronize(stream); CUERR;

        batchData.d_candidateReadIds.resize(totalNumValues);        

        if(totalNumValues == 0){
            cudaMemsetAsync(batchData.d_numCandidatesPerAnchor.get(), 0, sizeof(int) * batchData.numTasks , stream); CUERR;
            cudaMemsetAsync(batchData.d_numCandidatesPerAnchorPrefixSum.get(), 0, sizeof(int) * (1 + batchData.numTasks), stream); CUERR;
            batchData.totalNumCandidates = 0;
            return;
        }

        gpuMinhasher->retrieveValues(
            minhashHandle,
            nullptr,
            batchData.numTasks,              
            totalNumValues,
            batchData.d_candidateReadIds.get(),
            batchData.d_numCandidatesPerAnchor.get(),
            batchData.d_numCandidatesPerAnchorPrefixSum.get(),
            stream
        );

        cudaMemcpyAsync(
            &batchData.totalNumCandidates,
            batchData.d_numCandidatesPerAnchorPrefixSum.data() + batchData.numTasks,
            sizeof(int),
            D2H,
            stream
        ); CUERR;

        cudaStreamSynchronize(stream); CUERR;
    }

    const gpu::GpuMinhasher* gpuMinhasher{};
    mutable gpu::GpuMinhasher::QueryHandle minhashHandle;
};


struct GpuExtensionStepper{
public:
    int deviceId{};
    int insertSize{};
    int insertSizeStddev{};
    int maxextensionPerStep{};
    cub::CachingDeviceAllocator* cubAllocator{};
    const gpu::GpuReadStorage* gpuReadStorage{};
    const CorrectionOptions* correctionOptions{};
    const GoodAlignmentProperties* goodAlignmentProperties{};
    const cpu::QualityScoreConversion* qualityConversion{};
    mutable ReadStorageHandle readStorageHandle{};
    mutable gpu::KernelLaunchHandle kernelLaunchHandle{};
    
    GpuExtensionStepper() = default;

    GpuExtensionStepper(
        const gpu::GpuReadStorage& rs, 
        const CorrectionOptions& coropts,
        const GoodAlignmentProperties& gap,
        const cpu::QualityScoreConversion& qualityConversion_,
        int insertSize_,
        int insertSizeStddev_,
        int maxextensionPerStep_,
        cub::CachingDeviceAllocator& cubAllocator_
    ) : insertSize(insertSize_),
        insertSizeStddev(insertSizeStddev_),
        maxextensionPerStep(maxextensionPerStep_),
        cubAllocator(&cubAllocator_),
        gpuReadStorage(&rs),
        correctionOptions(&coropts),
        goodAlignmentProperties(&gap),
        qualityConversion(&qualityConversion_),
        readStorageHandle(gpuReadStorage->makeHandle()){

        cudaGetDevice(&deviceId); CUERR;

        kernelLaunchHandle = gpu::make_kernel_launch_handle(deviceId);
    }

    constexpr int getNumRefinementIterations() const noexcept{
        return 5;
    }

    void prepareStep(BatchData& batchData) const{
        const int numActiveTasks = batchData.indicesOfActiveTasks.size();
        batchData.numTasks = numActiveTasks;

        batchData.h_numAnchors.resize(1);
        batchData.d_numAnchors.resize(1);
        batchData.h_numCandidates.resize(1);
        batchData.d_numCandidates.resize(1);
        batchData.d_numCandidates2.resize(1);
        batchData.h_numAnchorsWithRemovedMates.resize(1);

        batchData.h_anchorReadIds.resize(numActiveTasks);
        batchData.d_anchorReadIds.resize(numActiveTasks);
        batchData.h_mateReadIds.resize(numActiveTasks);
        batchData.d_mateReadIds.resize(numActiveTasks);
        
        batchData.h_subjectSequencesData.resize(batchData.encodedSequencePitchInInts * numActiveTasks);
        batchData.d_subjectSequencesData.resize(batchData.encodedSequencePitchInInts * numActiveTasks);
        batchData.h_subjectSequencesDataDecoded.resize(batchData.decodedSequencePitchInBytes * numActiveTasks);
        batchData.d_subjectSequencesDataDecoded.resize(batchData.decodedSequencePitchInBytes * numActiveTasks);

        batchData.h_anchorSequencesLength.resize(numActiveTasks);
        batchData.d_anchorSequencesLength.resize(numActiveTasks);

        batchData.h_anchorQualityScores.resize(batchData.qualityPitchInBytes * numActiveTasks);
        batchData.d_anchorQualityScores.resize(batchData.qualityPitchInBytes * numActiveTasks);

        batchData.d_anchormatedata.resize(numActiveTasks * batchData.encodedSequencePitchInInts);

        batchData.h_inputanchormatedata.resize(numActiveTasks * batchData.encodedSequencePitchInInts);
        batchData.d_inputanchormatedata.resize(numActiveTasks * batchData.encodedSequencePitchInInts);

        batchData.h_numCandidatesPerAnchor.resize(numActiveTasks);
        batchData.d_numCandidatesPerAnchor.resize(numActiveTasks);
        batchData.h_numCandidatesPerAnchor2.resize(numActiveTasks);
        batchData.d_numCandidatesPerAnchor2.resize(numActiveTasks);
        batchData.h_numCandidatesPerAnchorPrefixSum.resize(numActiveTasks+1);
        batchData.d_numCandidatesPerAnchorPrefixSum.resize(numActiveTasks+1);
        batchData.d_numCandidatesPerAnchorPrefixSum2.resize(numActiveTasks+1);

        batchData.d_anchorIndicesWithRemovedMates.resize(numActiveTasks);

        batchData.h_numCandidatesPerAnchorPrefixSum[0] = 0;

        batchData.totalNumberOfUsedIds = 0;

        for(int t = 0; t < numActiveTasks; t++){
            auto& task = batchData.tasks[batchData.indicesOfActiveTasks[t]];
            task.dataIsAvailable = false;

            batchData.h_anchorReadIds[t] = task.myReadId;
            batchData.h_mateReadIds[t] = task.mateReadId;
            batchData.totalNumberOfUsedIds += task.allUsedCandidateReadIdPairs.size();

            // std::copy(
            //     task.encodedMate.begin(),
            //     task.encodedMate.end(),
            //     batchData.h_inputanchormatedata.begin() + t * batchData.encodedSequencePitchInInts
            // );

            //if(task.iteration >= 0){

                batchData.h_anchorSequencesLength[t] = task.currentAnchorLength;

                // std::copy(
                //     task.currentAnchor.begin(),
                //     task.currentAnchor.end(),
                //     batchData.h_subjectSequencesData.begin() + t * batchData.encodedSequencePitchInInts
                // );

                std::copy(
                    task.totalDecodedAnchors.back().begin(),
                    task.totalDecodedAnchors.back().end(),
                    batchData.h_subjectSequencesDataDecoded.begin() + t * batchData.decodedSequencePitchInBytes
                );
            // }else{
            //     //only hash kmers which include extended positions

            //     const int extendedPositionsPreviousIteration 
            //         = task.totalAnchorBeginInExtendedRead.at(task.iteration) - task.totalAnchorBeginInExtendedRead.at(task.iteration-1);

            //     const int lengthToHash = std::min(task.currentAnchorLength, kmerLength + extendedPositionsPreviousIteration - 1);
            //     batchData.h_anchorSequencesLength[t] = lengthToHash;

            //     //std::cerr << "lengthToHash = " << lengthToHash << "\n";

            //     std::vector<char> buf(task.currentAnchorLength);
            //     SequenceHelpers::decode2BitSequence(buf.data(), task.currentAnchor.data(), task.currentAnchorLength);
            //     SequenceHelpers::encodeSequence2Bit(
            //         batchData.h_subjectSequencesData.get() + t * batchData.encodedSequencePitchInInts, 
            //         buf.data() + task.currentAnchorLength - lengthToHash, 
            //         lengthToHash
            //     );
            // }

            assert(batchData.h_anchorQualityScores.size() >= (t+1) * batchData.qualityPitchInBytes);

            std::copy(
                task.currentQualityScores.begin(),
                task.currentQualityScores.end(),
                batchData.h_anchorQualityScores.begin() + t * batchData.qualityPitchInBytes
            );
        }

        // helpers::call_copy_n_kernel(
        //     thrust::make_zip_iterator(thrust::make_tuple(
        //         //batchData.h_inputanchormatedata.data(),
        //         batchData.h_subjectSequencesData.data()
        //     )),
        //     batchData.numTasks * batchData.encodedSequencePitchInInts,
        //     thrust::make_zip_iterator(thrust::make_tuple(
        //         //batchData.d_inputanchormatedata.data(),
        //         batchData.d_subjectSequencesData.data()
        //     )),
        //     batchData.streams[0]
        // );

        cudaMemcpyAsync(
            batchData.d_subjectSequencesDataDecoded.data(),
            batchData.h_subjectSequencesDataDecoded.data(),
            sizeof(char) * batchData.decodedSequencePitchInBytes * batchData.numTasks,
            H2D,
            batchData.streams[0]
        );

        helpers::call_copy_n_kernel(
            thrust::make_zip_iterator(thrust::make_tuple(
                //batchData.h_inputanchormatedata.data(),
                batchData.h_anchorQualityScores.data()
            )),
            batchData.numTasks * batchData.qualityPitchInBytes,
            thrust::make_zip_iterator(thrust::make_tuple(
                //batchData.d_inputanchormatedata.data(),
                batchData.d_anchorQualityScores.data()
            )),
            batchData.streams[0]
        );

        helpers::call_copy_n_kernel(
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.h_anchorSequencesLength.data(),
                batchData.h_anchorReadIds.data(),
                batchData.h_mateReadIds.data()
            )),
            batchData.numTasks,
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.d_anchorSequencesLength.data(),
                batchData.d_anchorReadIds.data(),
                batchData.d_mateReadIds.data()
            )),
            batchData.streams[0]
        );

        //2-bit encode anchorsequences
        helpers::lambda_kernel<<<SDIV(batchData.numTasks, (128 / 8)), 128, 0, batchData.streams[0]>>>(
            [
                decodedSequencePitchInBytes = batchData.decodedSequencePitchInBytes,
                encodedSequencePitchInInts = batchData.encodedSequencePitchInInts,
                numTasks = batchData.numTasks,
                encodedSequences = batchData.d_subjectSequencesData.data(),
                decodedSequences = batchData.d_subjectSequencesDataDecoded.data(),
                sequenceLengths = batchData.d_anchorSequencesLength.data()
            ] __device__ (){

                auto group = cg::tiled_partition<8>(cg::this_thread_block());
                const int numGroups = (blockDim.x * gridDim.x) / group.size();
                const int groupId = (threadIdx.x + blockIdx.x * blockDim.x) / group.size();

                for(int a = groupId; a < numTasks; a += numGroups){
                    unsigned int* const out = encodedSequences + a * encodedSequencePitchInInts;
                    const char* const in = decodedSequences + a * decodedSequencePitchInBytes;
                    const int length = sequenceLengths[a];

                    const int nInts = SequenceHelpers::getEncodedNumInts2Bit(length);
                    constexpr int basesPerInt = SequenceHelpers::basesPerInt2Bit();

                    for(int i = group.thread_rank(); i < nInts; i += group.size()){
                        unsigned int data = 0;

                        const int loopend = min((i+1) * basesPerInt, length);
                        
                        for(int nucIndex = i * basesPerInt; nucIndex < loopend; nucIndex++){
                            switch(in[nucIndex]) {
                            case 'A':
                                data = (data << 2) | SequenceHelpers::encodedbaseA();
                                break;
                            case 'C':
                                data = (data << 2) | SequenceHelpers::encodedbaseC();
                                break;
                            case 'G':
                                data = (data << 2) | SequenceHelpers::encodedbaseG();
                                break;
                            case 'T':
                                data = (data << 2) | SequenceHelpers::encodedbaseT();
                                break;
                            default:
                                data = (data << 2) | SequenceHelpers::encodedbaseA();
                                break;
                            }
                        }

                        if(i == nInts-1){
                            //pack bits of last integer into higher order bits
                            int leftoverbits = 2 * (nInts * basesPerInt - length);
                            if(leftoverbits > 0){
                                data <<= leftoverbits;
                            }
                        }

                        out[i] = data;
                    }
                }
            }
        );

        //2-bit encode anchorsequences 
        // helpers::lambda_kernel<<<batchData.numTasks, 64, 0, batchData.streams[0]>>>(
        //     [
        //         decodedSequencePitchInBytes = batchData.decodedSequencePitchInBytes,
        //         encodedSequencePitchInInts = batchData.encodedSequencePitchInInts,
        //         numTasks = batchData.numTasks,
        //         encodedSequences = batchData.d_subjectSequencesData.data(),
        //         decodedSequences = batchData.d_subjectSequencesDataDecoded.data(),
        //         sequenceLengths = batchData.d_anchorSequencesLength.data()
        //     ] __device__ (){

        //         for(int a = threadIdx.x + blockIdx.x * blockDim.x; a < numTasks; a += gridDim.x){
        //             unsigned int* const out = encodedSequences + a * encodedSequencePitchInInts;
        //             const char* const in = decodedSequences + a * decodedSequencePitchInBytes;
        //             const int length = sequenceLengths[a];

        //             SequenceHelpers::encodeSequence2Bit(out, in, length);
        //         }
        //     }
        // );

        if(0){
            batchData.h_usedReadIds.resize(batchData.totalNumberOfUsedIds);
            batchData.h_numUsedReadIdsPerAnchor.resize(batchData.numTasks);
            batchData.h_numUsedReadIdsPerAnchorPrefixSum.resize(batchData.numTasks);
            batchData.d_usedReadIds.resize(batchData.totalNumberOfUsedIds);
            batchData.d_numUsedReadIdsPerAnchor.resize(batchData.numTasks);
            batchData.d_numUsedReadIdsPerAnchorPrefixSum.resize(batchData.numTasks);

            batchData.d_segmentIdsOfUsedReadIds.resize(batchData.totalNumberOfUsedIds);
            batchData.h_segmentIdsOfUsedReadIds.resize(batchData.totalNumberOfUsedIds);
            
            batchData.h_numUsedReadIdsPerAnchorPrefixSum[0] = 0;

            auto segmentIdsIter = batchData.h_segmentIdsOfUsedReadIds.begin();
            auto h_usedReadIdsIter = batchData.h_usedReadIds.begin();

            for(int i = 0; i < batchData.numTasks; i++){
                auto& task = batchData.tasks[batchData.indicesOfActiveTasks[i]];

                const int numUsedIds = task.allUsedCandidateReadIdPairs.size();

                std::fill(segmentIdsIter, segmentIdsIter + numUsedIds, i);
                segmentIdsIter += numUsedIds;

                h_usedReadIdsIter = std::copy(
                    task.allUsedCandidateReadIdPairs.begin(),
                    task.allUsedCandidateReadIdPairs.end(),
                    h_usedReadIdsIter
                );
                batchData.h_numUsedReadIdsPerAnchor[i] = numUsedIds;

                if(i < batchData.numTasks - 1){
                    batchData.h_numUsedReadIdsPerAnchorPrefixSum[i+1] 
                        = batchData.h_numUsedReadIdsPerAnchorPrefixSum[i] + batchData.h_numUsedReadIdsPerAnchor[i];
                }
            }

            assert(std::distance(batchData.h_usedReadIds.data(), h_usedReadIdsIter) == batchData.totalNumberOfUsedIds);

            // cudaMemcpyAsync(
            //     batchData.d_segmentIdsOfUsedReadIds.data(),
            //     batchData.h_segmentIdsOfUsedReadIds.data(),
            //     sizeof(int) * batchData.totalNumberOfUsedIds,
            //     H2D,
            //     batchData.streams[1]
            // ); CUERR;

            // cudaMemcpyAsync(
            //     batchData.d_usedReadIds.data(),
            //     batchData.h_usedReadIds.data(),
            //     sizeof(read_number) * batchData.totalNumberOfUsedIds,
            //     H2D,
            //     batchData.streams[1]
            // ); CUERR;

            helpers::call_copy_n_kernel(
                thrust::make_zip_iterator(thrust::make_tuple(
                    batchData.h_segmentIdsOfUsedReadIds.data(),
                    batchData.h_usedReadIds.data()
                )),
                batchData.totalNumberOfUsedIds,
                thrust::make_zip_iterator(thrust::make_tuple(
                    batchData.d_segmentIdsOfUsedReadIds.data(),
                    batchData.d_usedReadIds.data()
                )),
                batchData.streams[1]
            );

            helpers::call_copy_n_kernel(
                thrust::make_zip_iterator(thrust::make_tuple(
                    batchData.h_numUsedReadIdsPerAnchorPrefixSum.data(),
                    batchData.h_numUsedReadIdsPerAnchor.data()
                )),
                batchData.numTasks,
                thrust::make_zip_iterator(thrust::make_tuple(
                    batchData.d_numUsedReadIdsPerAnchorPrefixSum.data(),
                    batchData.d_numUsedReadIdsPerAnchor.data()
                )),
                batchData.streams[1]
            );
        }
    }

    void step(BatchData& batchData) const{

        const int numActiveTasks = batchData.indicesOfActiveTasks.size();

        //std::cerr << "Step: Active tasks " << numActiveTasks << ", totalNumCandidates " << batchData.totalNumCandidates << "\n";

        nvtx::push_range("removeUsedIdsAndMateIds", 1);

        removeUsedIdsAndMateIdsCPU(batchData, batchData.streams[0], batchData.streams[1]);    
        // cudaStreamSynchronize(batchData.streams[0]);    
        // cudaStreamSynchronize(batchData.streams[1]);

        // removeUsedIdsAndMateIds(batchData, batchData.streams[0], batchData.streams[1]);  
        // cudaStreamSynchronize(batchData.streams[0]);    
        // cudaStreamSynchronize(batchData.streams[1]);  

        // std::vector<read_number> foo(batchData.totalNumCandidates);
        // cudaMemcpy(
        //     foo.data(),
        //     batchData.d_candidateReadIds.data(),
        //     sizeof(read_number) * batchData.totalNumCandidates,
        //     D2H
        // );

        // std::vector<int> foo2(batchData.numTasks);
        // cudaMemcpy(
        //     foo2.data(),
        //     batchData.d_numCandidatesPerAnchor.data(),
        //     sizeof(int) * batchData.numTasks,
        //     D2H
        // );

        // std::vector<int> foo3(batchData.numTasks+1);
        // cudaMemcpy(
        //     foo3.data(),
        //     batchData.d_numCandidatesPerAnchorPrefixSum.data(),
        //     sizeof(int) * (batchData.numTasks+1),
        //     D2H
        // );

        // std::cerr << "foo\n";
        // std::copy(foo.begin(), foo.end(), std::ostream_iterator<read_number>(std::cerr, " "));
        // std::cerr << "\n";
        // std::cerr << "foo2\n";
        // std::copy(foo2.begin(), foo2.end(), std::ostream_iterator<int>(std::cerr, " "));
        // std::cerr << "\n";
        // std::cerr << "foo3\n";
        // std::copy(foo3.begin(), foo3.end(), std::ostream_iterator<int>(std::cerr, " "));
        // std::cerr << "\n";

        nvtx::pop_range();

        //std::exit(0);

        nvtx::push_range("flagpairs", 7);

        computePairFlagsCpu(batchData, batchData.streams[0]);

        nvtx::pop_range();


        //collectTimer.start();

        nvtx::push_range("loadCandidateSequenceData", 2);

        loadCandidateSequenceData(batchData, batchData.streams[0]);

        nvtx::pop_range();
        
        if(batchData.numTasksWithMateRemoved > 0){

            //for those tasks where a mate id has been removed, remove candidates whose sequence is equal to the mate sequence.
            //Sets batchData.totalNumCandidates to the sum of number of candidates for all tasks.

            nvtx::push_range("eraseDataOfRemovedMates", 3);

            eraseDataOfRemovedMates(batchData, batchData.streams[0]);

            nvtx::pop_range();

        }

        //collectTimer.stop();

        /*
            Compute alignments
        */

        //alignmentTimer.start();

        nvtx::push_range("calculateAlignments", 4);

        calculateAlignments(batchData, batchData.streams[0]);

        nvtx::pop_range();

        //alignmentTimer.stop();
        
        nvtx::push_range("filterAlignments", 5);
    
        //Sets batchData.totalNumCandidates to the sum of number of candidates for all tasks.
        filterAlignments(batchData, batchData.streams[0]);

        nvtx::pop_range();

        //loadCandidateQualityScores(batchData, batchData.streams[0]);

        nvtx::push_range("computeMSAs", 6);

        //Sets batchData.totalNumCandidates to the sum of number of candidates for all tasks. (msa refinement can remove candidates)
        computeMSAs(batchData, batchData.streams[0], batchData.streams[1]);

        nvtx::pop_range();

        nvtx::push_range("computeExtendedSequences", 7);

        computeExtendedSequencesFromMSAs(batchData, batchData.streams[0]);

        nvtx::pop_range();

        //copy all necessary buffers to host
            
        nvtx::push_range("copyBuffersToHost", 8);

        copyBuffersToHost(batchData, batchData.streams[0], batchData.streams[1]);

        nvtx::pop_range();
        

        cudaStreamSynchronize(batchData.streams[0]); CUERR;
        cudaStreamSynchronize(batchData.streams[1]); CUERR;        
    }

    void extendAfterStep(BatchData& batchData) const{

        const int numActiveTasks = batchData.indicesOfActiveTasks.size();

        // for(int i = 0; i < numActiveTasks; i++){
        //     auto& task = batchData.tasks[batchData.indicesOfActiveTasks[i]];

        //     task.numRemainingCandidates = batchData.h_numCandidatesPerAnchor[i];

        //     if(task.numRemainingCandidates == 0){
        //         task.abort = true;
        //         task.abortReason = AbortReason::NoPairedCandidatesAfterAlignment;
        //     }
        // }

        nvtx::push_range("Unpack gpu results", 6);

        for(int i = 0; i < numActiveTasks; i++){ 
            const int indexOfActiveTask = batchData.indicesOfActiveTasks[i];
            auto& task = batchData.tasks[indexOfActiveTask];

            // if(task.numRemainingCandidates == 0){
            //     continue;
            // }
            // assert(task.numRemainingCandidates > 0);

            task.numRemainingCandidates = batchData.h_numCandidatesPerAnchor[i];

            task.abortReason = batchData.h_abortReasons[i];
            if(task.abortReason == AbortReason::None){
                task.mateHasBeenFound = batchData.h_outputMateHasBeenFound[i];

                if(!task.mateHasBeenFound){
                    const int newlength = batchData.h_outputAnchorLengths[i];

                    std::string newseq(batchData.h_outputAnchors.data() + i * batchData.outputAnchorPitchInBytes, newlength);
                    std::string newq(batchData.h_outputAnchorQualities.data() + i * batchData.outputAnchorQualityPitchInBytes, newlength);

                    task.currentAnchorLength = newlength;
                    task.accumExtensionLengths = batchData.h_accumExtensionsLengths[i];
                    task.totalDecodedAnchors.emplace_back(std::move(newseq));
                    task.totalAnchorQualityScores.emplace_back(std::move(newq));
                    task.totalAnchorBeginInExtendedRead.emplace_back(task.accumExtensionLengths);

                    task.currentQualityScores = task.totalAnchorQualityScores.back(); 
                    
                }else{
                    const int sizeofGap = batchData.h_sizeOfGapToMate[i];
                    if(sizeofGap == 0){
                        task.accumExtensionLengths = batchData.h_accumExtensionsLengths[i];
                        task.totalAnchorBeginInExtendedRead.emplace_back(task.accumExtensionLengths);
                        task.totalDecodedAnchors.emplace_back(task.decodedMateRevC);
                        task.totalAnchorQualityScores.emplace_back(task.mateQualityScoresReversed);
                    }else{
                        const int newlength = batchData.h_outputAnchorLengths[i];

                        std::string newseq(batchData.h_outputAnchors.data() + i * batchData.outputAnchorPitchInBytes, newlength);
                        std::string newq(batchData.h_outputAnchorQualities.data() + i * batchData.outputAnchorQualityPitchInBytes, newlength);

                        task.accumExtensionLengths = batchData.h_accumExtensionsLengths[i];
                        task.totalDecodedAnchors.emplace_back(std::move(newseq));
                        task.totalAnchorQualityScores.emplace_back(std::move(newq));
                        task.totalAnchorBeginInExtendedRead.emplace_back(task.accumExtensionLengths);

                        task.accumExtensionLengths += newlength;
                        task.totalAnchorBeginInExtendedRead.emplace_back(task.accumExtensionLengths);
                        task.totalDecodedAnchors.emplace_back(task.decodedMateRevC);
                        task.totalAnchorQualityScores.emplace_back(task.mateQualityScoresReversed);
                    }
                }
            }

            task.abort = task.abortReason != AbortReason::None;
        }

        nvtx::pop_range();

        // nvtx::push_range("Encode remaining anchors", 6);

        // for(int i = 0; i < numActiveTasks; i++){ 
        //     const int indexOfActiveTask = batchData.indicesOfActiveTasks[i];
        //     auto& task = batchData.tasks[indexOfActiveTask];

        //     if(task.numRemainingCandidates == 0){
        //         continue;
        //     }
        //     assert(task.numRemainingCandidates > 0);

        //     if(task.abortReason == AbortReason::None){
        //         if(!task.mateHasBeenFound){
        //             const int numInts = SequenceHelpers::getEncodedNumInts2Bit(task.currentAnchorLength);
        //             task.currentAnchor.resize(numInts);

        //             SequenceHelpers::encodeSequence2Bit(
        //                 task.currentAnchor.data(), 
        //                 task.totalDecodedAnchors.back().data(), 
        //                 task.currentAnchorLength
        //             );
        //         }
        //     }
        // }

        // nvtx::pop_range();

        assert(batchData.tasks.size() / 4 == batchData.numReadPairs);

        for(int i = 0; i < numActiveTasks; i++){ 
            const int indexOfActiveTask = batchData.indicesOfActiveTasks[i];
            const auto& task = batchData.tasks[indexOfActiveTask];

            const int whichtype = task.id % 4;

            assert(indexOfActiveTask % 4 == whichtype);

            if(whichtype == 0){
                assert(task.direction == ExtensionDirection::LR);
                assert(task.pairedEnd == true);

                if(task.mateHasBeenFound){                    
                    batchData.tasks[indexOfActiveTask + 1].abort = true;
                    batchData.tasks[indexOfActiveTask + 1].abortReason = AbortReason::PairedAnchorFinished;
                    batchData.tasks[indexOfActiveTask + 3].abort = true;
                    batchData.tasks[indexOfActiveTask + 3].abortReason = AbortReason::OtherStrandFoundMate;
                }else if(task.abort){
                    batchData.tasks[indexOfActiveTask + 1].abort = true;
                    batchData.tasks[indexOfActiveTask + 1].abortReason = AbortReason::PairedAnchorFinished;
                }
            }else if(whichtype == 2){
                assert(task.direction == ExtensionDirection::RL);
                assert(task.pairedEnd == true);

                if(task.mateHasBeenFound){                    
                    batchData.tasks[indexOfActiveTask - 1].abort = true;
                    batchData.tasks[indexOfActiveTask - 1].abortReason = AbortReason::OtherStrandFoundMate;
                    batchData.tasks[indexOfActiveTask + 1].abort = true;
                    batchData.tasks[indexOfActiveTask + 1].abortReason = AbortReason::PairedAnchorFinished;
                }else if(task.abort){
                    batchData.tasks[indexOfActiveTask + 1].abort = true;
                    batchData.tasks[indexOfActiveTask + 1].abortReason = AbortReason::PairedAnchorFinished;
                }
            }
        }


        /*
            update book-keeping of used candidates
        */  
        nvtx::push_range("usedcandidates", 6);
        for(int i = 0; i < numActiveTasks; i++){
            auto& task = batchData.tasks[batchData.indicesOfActiveTasks[i]];

                                    
            {
                if(task.dataIsAvailable){
                    std::vector<read_number> tmp(task.allUsedCandidateReadIdPairs.size() + task.candidateReadIds.size());
                    auto tmp_end = std::merge(
                        task.allUsedCandidateReadIdPairs.begin(),
                        task.allUsedCandidateReadIdPairs.end(),
                        task.candidateReadIds.begin(),
                        task.candidateReadIds.end(),
                        tmp.begin()
                    );

                    tmp.erase(tmp_end, tmp.end());

                    std::swap(task.allUsedCandidateReadIdPairs, tmp);
                }else{
                    const int numCandidates = batchData.h_numCandidatesPerAnchor[i];
                    const int offset = batchData.h_numCandidatesPerAnchorPrefixSum[i];
                    const read_number* ids = &batchData.h_candidateReadIds[offset];

                    std::vector<read_number> tmp(task.allUsedCandidateReadIdPairs.size() + numCandidates);
                    auto tmp_end = std::merge(
                        task.allUsedCandidateReadIdPairs.begin(),
                        task.allUsedCandidateReadIdPairs.end(),
                        ids,
                        ids + numCandidates,
                        tmp.begin()
                    );

                    tmp.erase(tmp_end, tmp.end());

                    std::swap(task.allUsedCandidateReadIdPairs, tmp);
                }
            }

            // task.usedCandidateReadIdsPerIteration.emplace_back(std::move(task.candidateReadIds));
            // task.usedAlignmentsPerIteration.emplace_back(std::move(task.alignments));
            // task.usedAlignmentFlagsPerIteration.emplace_back(std::move(task.alignmentFlags));

            task.iteration++;
        }

        nvtx::pop_range();
        
        //update list of active task indices

       
        batchData.indicesOfActiveTasks.erase(
            std::remove_if(
                batchData.indicesOfActiveTasks.begin(), 
                batchData.indicesOfActiveTasks.end(),
                [&](int index){
                    return !batchData.tasks[index].isActive(insertSize, insertSizeStddev);
                }
            ),
            batchData.indicesOfActiveTasks.end()
        );
    }


    std::vector<ExtendResult> constructResults(BatchData& batchData) const{
        std::vector<ExtendResult> extendResults;
        extendResults.reserve(batchData.tasks.size());

        for(const auto& task : batchData.tasks){

            ExtendResult extendResult;
            extendResult.direction = task.direction;
            extendResult.numIterations = task.iteration;
            extendResult.aborted = task.abort;
            extendResult.abortReason = task.abortReason;
            extendResult.readId1 = task.myReadId;
            extendResult.readId2 = task.mateReadId;
            extendResult.originalLength = task.myLength;
            extendResult.originalMateLength = task.mateLength;
            extendResult.read1begin = 0;

            //construct extended read
            //build msa of all saved totalDecodedAnchors[0]

            const int numsteps = task.totalDecodedAnchors.size();

            int maxlen = 0;
            for(const auto& s: task.totalDecodedAnchors){
                const int len = s.length();
                if(len > maxlen){
                    maxlen = len;
                }
            }

            const std::string& decodedAnchor = task.totalDecodedAnchors[0];
            const std::string& anchorQuality = task.totalAnchorQualityScores[0];

            const std::vector<int> shifts(task.totalAnchorBeginInExtendedRead.begin() + 1, task.totalAnchorBeginInExtendedRead.end());
            std::vector<float> initialWeights(numsteps-1, 1.0f);


            std::vector<char> stepstrings(maxlen * (numsteps-1), '\0');
            std::vector<char> stepqualities(maxlen * (numsteps-1), '\0');
            std::vector<int> stepstringlengths(numsteps-1);
            for(int c = 1; c < numsteps; c++){
                std::copy(
                    task.totalDecodedAnchors[c].begin(),
                    task.totalDecodedAnchors[c].end(),
                    stepstrings.begin() + (c-1) * maxlen
                );
                assert(task.totalAnchorQualityScores[c].size() <= maxlen);
                std::copy(
                    task.totalAnchorQualityScores[c].begin(),
                    task.totalAnchorQualityScores[c].end(),
                    stepqualities.begin() + (c-1) * maxlen
                );
                stepstringlengths[c-1] = task.totalDecodedAnchors[c].size();
            }

            MultipleSequenceAlignment::InputData msaInput;
            msaInput.useQualityScores = false;
            msaInput.subjectLength = decodedAnchor.length();
            msaInput.nCandidates = numsteps-1;
            msaInput.candidatesPitch = maxlen;
            msaInput.candidateQualitiesPitch = maxlen;
            msaInput.subject = decodedAnchor.c_str();
            msaInput.candidates = stepstrings.data();
            msaInput.subjectQualities = anchorQuality.data();
            msaInput.candidateQualities = stepqualities.data();
            msaInput.candidateLengths = stepstringlengths.data();
            msaInput.candidateShifts = shifts.data();
            msaInput.candidateDefaultWeightFactors = initialWeights.data();

            MultipleSequenceAlignment msa(qualityConversion);

            msa.build(msaInput);

            //msa.print(std::cerr);

            extendResult.success = true;

            std::string extendedRead(msa.consensus.begin(), msa.consensus.end());
            std::string extendedReadQuality(msa.consensus.size(), '\0');
            std::transform(msa.support.begin(), msa.support.end(), extendedReadQuality.begin(),
                [](const float f){
                    return getQualityChar(f);
                }
            );

            std::copy(decodedAnchor.begin(), decodedAnchor.end(), extendedRead.begin());
            std::copy(anchorQuality.begin(), anchorQuality.end(), extendedReadQuality.begin());

            if(task.mateHasBeenFound){
                //std::cerr << "copy " << task.decodedMateRevC << " to end of consensus " << task.myReadId << "\n";
                std::copy(
                    task.decodedMateRevC.begin(),
                    task.decodedMateRevC.end(),
                    extendedRead.begin() + extendedRead.length() - task.decodedMateRevC.length()
                );

                std::copy(
                    task.mateQualityScoresReversed.begin(),
                    task.mateQualityScoresReversed.end(),
                    extendedReadQuality.begin() + extendedReadQuality.length() - task.decodedMateRevC.length()
                );

                extendResult.read2begin = extendedRead.length() - task.decodedMateRevC.length();
            }else{
                extendResult.read2begin = -1;
            }

            extendResult.extendedRead = std::move(extendedRead);
            extendResult.qualityScores = std::move(extendedReadQuality);

            extendResult.mateHasBeenFound = task.mateHasBeenFound;

            extendResults.emplace_back(std::move(extendResult));
        }

        #if 0

        std::vector<ExtendResult> extendResultsCombined = ReadExtenderBase::combinePairedEndDirectionResults(
            extendResults,
            insertSize,
            insertSizeStddev
        );

        #else

        std::vector<ExtendResult> extendResultsCombined = ReadExtenderBase::combinePairedEndDirectionResults4(
            extendResults,
            insertSize,
            insertSizeStddev
        );

        #endif

        return extendResultsCombined;
    }


    void computePairFlagsCpu(BatchData& batchData, cudaStream_t stream) const{
        //computed in removeUsedIdsAndMateIdsCPU
        
        // cudaMemcpyAsync(
        //     batchData.h_numCandidatesPerAnchorPrefixSum.data(),
        //     batchData.d_numCandidatesPerAnchorPrefixSum.data(),
        //     sizeof(int) * (batchData.numTasks + 1),
        //     D2H,
        //     stream
        // );

        // cudaMemcpyAsync(
        //     batchData.h_candidateReadIds.data(),
        //     batchData.d_candidateReadIds.data(),
        //     sizeof(read_number) * batchData.totalNumCandidates,
        //     D2H,
        //     stream
        // );

        batchData.h_isPairedCandidate.resize(batchData.totalNumCandidates);
        batchData.d_isPairedCandidate.resize(batchData.totalNumCandidates);

        std::fill(batchData.h_isPairedCandidate.begin(), batchData.h_isPairedCandidate.end(), false);
        // cudaMemcpyAsync(
        //     batchData.d_isPairedCandidate.data(),
        //     batchData.h_isPairedCandidate.data(),
        //     sizeof(bool) * batchData.totalNumCandidates,
        //     H2D,
        //     stream
        // ); CUERR;

        // return;

        std::vector<int> numPairedPerAnchor(batchData.numTasks, 0);

        //cudaStreamSynchronize(stream); CUERR;

        #if 0 //this assumes all active tasks are paired
        for(int ap = 0; ap < batchData.numTasks / 2; ap++){
            const int begin1 = batchData.h_numCandidatesPerAnchorPrefixSum[2*ap + 0];
            const int end1 = batchData.h_numCandidatesPerAnchorPrefixSum[2*ap + 1];
            const int begin2 = batchData.h_numCandidatesPerAnchorPrefixSum[2*ap + 1];
            const int end2 = batchData.h_numCandidatesPerAnchorPrefixSum[2*ap + 2];

            // assert(std::is_sorted(pairIds + begin1, pairIds + end1));
            // assert(std::is_sorted(pairIds + begin2, pairIds + end2));

            std::vector<int> pairedPositions(std::min(end1-begin1, end2-begin2));
            std::vector<int> pairedPositions2(std::min(end1-begin1, end2-begin2));

            auto endIters = findPositionsOfPairedReadIds(
                batchData.h_candidateReadIds.data() + begin1,
                batchData.h_candidateReadIds.data() + end1,
                batchData.h_candidateReadIds.data() + begin2,
                batchData.h_candidateReadIds.data() + end2,
                pairedPositions.begin(),
                pairedPositions2.begin()
            );

            pairedPositions.erase(endIters.first, pairedPositions.end());
            pairedPositions2.erase(endIters.second, pairedPositions2.end());
            for(auto i : pairedPositions){
                batchData.h_isPairedCandidate[begin1 + i] = true;
            }
            for(auto i : pairedPositions2){
                batchData.h_isPairedCandidate[begin2 + i] = true;
            }

            numPairedPerAnchor[2*ap + 0] = pairedPositions.size();
            numPairedPerAnchor[2*ap + 1] = pairedPositions2.size();                
        }
        #else //this checks if task of anchor mate is active

        for(int first = 0, second = 1; second < batchData.numTasks; ){
            const int taskindex1 = batchData.indicesOfActiveTasks[first];
            const int taskindex2 = batchData.indicesOfActiveTasks[second];

            const bool areConsecutiveTasks = batchData.tasks[taskindex1].id + 1 == batchData.tasks[taskindex2].id;
            const bool arePairedTasks = (batchData.tasks[taskindex1].id % 2) + 1 == (batchData.tasks[taskindex2].id % 2);

            if(areConsecutiveTasks && arePairedTasks){
                const int begin1 = batchData.h_numCandidatesPerAnchorPrefixSum[first];
                const int end1 = batchData.h_numCandidatesPerAnchorPrefixSum[second];
                const int begin2 = batchData.h_numCandidatesPerAnchorPrefixSum[second];
                const int end2 = batchData.h_numCandidatesPerAnchorPrefixSum[second + 1];

                // assert(std::is_sorted(pairIds + begin1, pairIds + end1));
                // assert(std::is_sorted(pairIds + begin2, pairIds + end2));

                std::vector<int> pairedPositions(std::min(end1-begin1, end2-begin2));
                std::vector<int> pairedPositions2(std::min(end1-begin1, end2-begin2));

                auto endIters = findPositionsOfPairedReadIds(
                    batchData.h_candidateReadIds.data() + begin1,
                    batchData.h_candidateReadIds.data() + end1,
                    batchData.h_candidateReadIds.data() + begin2,
                    batchData.h_candidateReadIds.data() + end2,
                    pairedPositions.begin(),
                    pairedPositions2.begin()
                );

                pairedPositions.erase(endIters.first, pairedPositions.end());
                pairedPositions2.erase(endIters.second, pairedPositions2.end());
                for(auto i : pairedPositions){
                    batchData.h_isPairedCandidate[begin1 + i] = true;
                }
                for(auto i : pairedPositions2){
                    batchData.h_isPairedCandidate[begin2 + i] = true;
                }

                numPairedPerAnchor[first] = pairedPositions.size();
                numPairedPerAnchor[second] = pairedPositions2.size();
                
                first += 2; second += 2;
            }else{
                first += 1; second += 1;
            }
        }
        

        #endif

        cudaMemcpyAsync(
            batchData.d_isPairedCandidate.data(),
            batchData.h_isPairedCandidate.data(),
            sizeof(bool) * batchData.totalNumCandidates,
            H2D,
            stream
        ); CUERR;
    }


    void removeUsedIdsAndMateIdsCPU(BatchData& batchData, cudaStream_t firstStream, cudaStream_t secondStream) const{
        batchData.h_candidateReadIds.resize(batchData.totalNumCandidates);
        batchData.h_numCandidatesPerAnchor.resize(batchData.numTasks);
        batchData.h_numCandidatesPerAnchorPrefixSum.resize(batchData.numTasks + 1);
        batchData.h_candidateReadIds2.resize(batchData.totalNumCandidates);
        batchData.h_numCandidatesPerAnchor2.resize(batchData.numTasks);
        batchData.h_numCandidatesPerAnchorPrefixSum2.resize(batchData.numTasks + 1);

        cudaMemcpyAsync(
            batchData.h_candidateReadIds.data(),
            batchData.d_candidateReadIds.data(),
            sizeof(read_number) * batchData.totalNumCandidates,
            D2H,
            firstStream
        );
        
        // cudaMemcpyAsync(
        //     batchData.h_numCandidatesPerAnchor.data(),
        //     batchData.d_numCandidatesPerAnchor.data(),
        //     sizeof(int) * (batchData.numTasks),
        //     D2H,
        //     firstStream
        // );
        // cudaMemcpyAsync(
        //     batchData.h_numCandidatesPerAnchorPrefixSum.data(),
        //     batchData.d_numCandidatesPerAnchorPrefixSum.data(),
        //     sizeof(int) * (batchData.numTasks + 1),
        //     D2H,
        //     firstStream
        // );
        helpers::call_copy_n_kernel(
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.d_numCandidatesPerAnchorPrefixSum.data() + 1,
                batchData.d_numCandidatesPerAnchor.data()
            )),
            batchData.numTasks,
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.h_numCandidatesPerAnchorPrefixSum.data() + 1,
                batchData.h_numCandidatesPerAnchor.data()
            )),
            firstStream
        ); 
        cudaStreamSynchronize(firstStream);

        read_number* destids = batchData.h_candidateReadIds2.data();
        batchData.h_numCandidatesPerAnchorPrefixSum[0] = 0;
        batchData.h_numCandidatesPerAnchorPrefixSum2[0] = 0;
        batchData.numTasksWithMateRemoved = 0;

        /*
            Remove anchor ids and mate ids from candidates
        */

        for(int k = 0; k < batchData.numTasks; k++){
            auto& task = batchData.tasks[batchData.indicesOfActiveTasks[k]];

            int num = batchData.h_numCandidatesPerAnchor[k];
            const int offset = batchData.h_numCandidatesPerAnchorPrefixSum[k];
            read_number* myIds = batchData.h_candidateReadIds.data() + offset;

            auto readIdPos = std::lower_bound(
                myIds,                                            
                myIds + num,
                task.myReadId
            );

            if(readIdPos != myIds + num && *readIdPos == task.myReadId){
                std::copy(readIdPos + 1, myIds + num, readIdPos);
                num--;
            }

            if(task.pairedEnd){                

                //remove mate of input from candidate list
                auto mateReadIdPos = std::lower_bound(
                    myIds,                                            
                    myIds + num,
                    task.mateReadId
                );

                if(mateReadIdPos != myIds + num && *mateReadIdPos == task.mateReadId){
                    batchData.h_numCandidatesPerAnchor2[k] = num - 1;
                    destids = std::copy(myIds, mateReadIdPos, destids);
                    destids = std::copy(mateReadIdPos + 1, myIds + num, destids);
                    task.mateRemovedFromCandidates = true;
                    batchData.numTasksWithMateRemoved++;
                }else{
                    batchData.h_numCandidatesPerAnchor2[k] = num;
                    destids = std::copy(myIds, myIds + num, destids);
                    task.mateRemovedFromCandidates = false;
                }

                batchData.h_numCandidatesPerAnchorPrefixSum2[k+1] =
                    batchData.h_numCandidatesPerAnchorPrefixSum2[k] + batchData.h_numCandidatesPerAnchor2[k];
            }else{
                batchData.h_numCandidatesPerAnchor2[k] = num;
                destids = std::copy(myIds, myIds + num, destids);
                task.mateRemovedFromCandidates = false;
                batchData.h_numCandidatesPerAnchorPrefixSum2[k+1] =
                    batchData.h_numCandidatesPerAnchorPrefixSum2[k] + batchData.h_numCandidatesPerAnchor2[k];
            }
        }

        /*
            Remove candidate pairs which have already been used for extension
        */

        destids = batchData.h_candidateReadIds.data();

        for(int k = 0; k < batchData.numTasks; k++){
            auto& task = batchData.tasks[batchData.indicesOfActiveTasks[k]];

            const int num = batchData.h_numCandidatesPerAnchor2[k];
            const int offset = batchData.h_numCandidatesPerAnchorPrefixSum2[k];
            read_number* myIds = batchData.h_candidateReadIds2.data() + offset;

            std::vector<read_number> tmp(task.candidateReadIds.size());

            #if 0
            auto end = std::set_difference(
                myIds,
                myIds + num,
                task.allUsedCandidateReadIdPairs.begin(),
                task.allUsedCandidateReadIdPairs.end(),
                destids
            );
            #else
            //remove none
            auto end = std::copy(myIds, myIds + num, destids);
            #endif

            batchData.h_numCandidatesPerAnchor[k] = std::distance(destids, end);
            destids = end;

            batchData.h_numCandidatesPerAnchorPrefixSum[k+1] =
                    batchData.h_numCandidatesPerAnchorPrefixSum[k] + batchData.h_numCandidatesPerAnchor[k];
        }

        batchData.totalNumCandidates = batchData.h_numCandidatesPerAnchorPrefixSum[batchData.numTasks];

        //std::cerr << "old numTasksWithMateRemoved = " << batchData.numTasksWithMateRemoved << ", totalNumCandidates = " << batchData.totalNumCandidates << "\n";

        cudaMemcpyAsync(
            batchData.d_candidateReadIds.data(),
            batchData.h_candidateReadIds.data(),
            sizeof(read_number) * batchData.totalNumCandidates,
            H2D,
            firstStream
        );
        // cudaMemcpyAsync(
        //     batchData.d_numCandidatesPerAnchor.data(),
        //     batchData.h_numCandidatesPerAnchor.data(),
        //     sizeof(int) * (batchData.numTasks),
        //     H2D,
        //     firstStream
        // );
        // cudaMemcpyAsync(
        //     batchData.d_numCandidatesPerAnchorPrefixSum.data(),
        //     batchData.h_numCandidatesPerAnchorPrefixSum.data(),
        //     sizeof(int) * (batchData.numTasks + 1),
        //     H2D,
        //     firstStream
        // );

        helpers::call_copy_n_kernel(
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.h_numCandidatesPerAnchorPrefixSum.data() + 1,
                batchData.h_numCandidatesPerAnchor.data()
            )),
            batchData.numTasks,
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.d_numCandidatesPerAnchorPrefixSum.data() + 1,
                batchData.d_numCandidatesPerAnchor.data()
            )),
            firstStream
        ); 

        batchData.d_anchorIndicesOfCandidates.resize(batchData.totalNumCandidates);
        batchData.d_anchorIndicesOfCandidates2.resize(batchData.totalNumCandidates);
        batchData.h_anchorIndicesOfCandidates.resize(batchData.totalNumCandidates);

        for(int i = 0, sum = 0; i < batchData.numTasks; i++){
            std::fill(
                batchData.h_anchorIndicesOfCandidates.data() + sum,
                batchData.h_anchorIndicesOfCandidates.data() + sum + batchData.h_numCandidatesPerAnchor[i],
                i
            );
            sum += batchData.h_numCandidatesPerAnchor[i];
        }

        cudaMemcpyAsync(
            batchData.d_anchorIndicesOfCandidates.data(),
            batchData.h_anchorIndicesOfCandidates.data(),
            sizeof(int) * batchData.totalNumCandidates,
            H2D,
            firstStream
        ); CUERR;

        if(batchData.numTasksWithMateRemoved > 0){

            batchData.h_inputanchormatedata.resize(batchData.encodedSequencePitchInInts * batchData.numTasksWithMateRemoved);
            batchData.h_segmentIdsOfReadIds.resize(batchData.numTasksWithMateRemoved);

            unsigned int* destmatedata = batchData.h_inputanchormatedata.data();
            int* destids = batchData.h_segmentIdsOfReadIds.data();

            for(int k = 0; k < batchData.numTasks; k++){
                auto& task = batchData.tasks[batchData.indicesOfActiveTasks[k]];
                if(task.mateRemovedFromCandidates){
                    destmatedata = std::copy(
                        task.encodedMate.begin(),
                        task.encodedMate.end(),
                        destmatedata
                    );
                    *destids = k;
                    destids++;
                }
            }

            cudaMemcpyAsync(
                batchData.d_anchormatedata.data(),
                batchData.h_inputanchormatedata.data(),
                sizeof(unsigned int) * (batchData.encodedSequencePitchInInts * batchData.numTasksWithMateRemoved),
                H2D,
                firstStream
            );

            cudaMemcpyAsync(
                batchData.d_anchorIndicesWithRemovedMates.data(),
                batchData.h_segmentIdsOfReadIds.data(),
                sizeof(int) * (batchData.numTasksWithMateRemoved),
                H2D,
                firstStream
            );
        }

    }


    void removeUsedIdsAndMateIds(BatchData& batchData, cudaStream_t firstStream, cudaStream_t secondStream) const{
        //std::cerr << "\n" << batchData.totalNumCandidates << "\n";
        
        batchData.d_anchorIndicesOfCandidates.resize(batchData.totalNumCandidates);
        batchData.d_anchorIndicesOfCandidates2.resize(batchData.totalNumCandidates);

        batchData.d_candidateReadIds2.resize(batchData.totalNumCandidates);

        batchData.h_segmentIdsOfReadIds.resize(batchData.totalNumCandidates);

        batchData.h_numCandidatesPerAnchor.resize(batchData.numTasks);

        bool* d_shouldBeKept = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_shouldBeKept, sizeof(bool) * batchData.totalNumCandidates, firstStream);   

        bool* d_anchorFlags = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_anchorFlags, sizeof(bool) * batchData.numTasks, firstStream);   

        //determine required temp bytes for following cub calls, and allocate temp storage

        cudaError_t cubstatus = cudaSuccess;
        std::size_t cubBytes = 0;
        std::size_t cubBytes2 = 0;

        cubstatus = cub::DeviceSelect::Flagged(
            nullptr,
            cubBytes2,
            batchData.d_candidateReadIds.data(),
            d_shouldBeKept,
            batchData.d_candidateReadIds2.data(),
            batchData.h_numCandidates.data(),
            batchData.totalNumCandidates,
            firstStream
        );
        assert(cubstatus == cudaSuccess);
        
        cubBytes = std::max(cubBytes, cubBytes2);
        
        cubstatus = cub::DeviceScan::InclusiveSum(
            nullptr, 
            cubBytes2, 
            (int*)nullptr,
            (int*)nullptr,
            batchData.numTasks,
            firstStream
        );
        assert(cubstatus == cudaSuccess);
        
        cubBytes = std::max(cubBytes, cubBytes2);
        
        cubstatus = cub::DeviceScan::InclusiveScan(
            nullptr, 
            cubBytes2, 
            batchData.d_anchorIndicesOfCandidates.data(), 
            batchData.d_anchorIndicesOfCandidates.data(), 
            cub::Max{},
            batchData.totalNumCandidates,
            firstStream
        );
        assert(cubstatus == cudaSuccess);
        
        cubBytes = std::max(cubBytes, cubBytes2);
        
        cubstatus = cub::DeviceSelect::Flagged(
            nullptr,
            cubBytes2,
            thrust::make_counting_iterator(0),
            d_anchorFlags,
            batchData.d_anchorIndicesWithRemovedMates.data(),
            batchData.h_numAnchorsWithRemovedMates.data(),
            batchData.numTasks,
            firstStream
        );
        assert(cubstatus == cudaSuccess);
        
        cubBytes = std::max(cubBytes, cubBytes2);
        
        void* cubtempstorage; cubAllocator->DeviceAllocate((void**)&cubtempstorage, cubBytes, firstStream);   
        
        helpers::call_fill_kernel_async(d_shouldBeKept, batchData.totalNumCandidates, false, firstStream);
        
        //flag candidates ids to remove because they are equal to anchor id or equal to mate id
        readextendergpukernels::flagCandidateIdsWhichAreEqualToAnchorOrMateKernel<<<4096, 128, 0, firstStream>>>(
            batchData.d_candidateReadIds.data(),
            batchData.d_anchorReadIds.data(),
            batchData.d_mateReadIds.data(),
            batchData.d_numCandidatesPerAnchorPrefixSum.data(),
            batchData.d_numCandidatesPerAnchor.data(),
            d_shouldBeKept,
            d_anchorFlags,
            batchData.d_numCandidatesPerAnchor2.data(),
            batchData.numTasks,
            batchData.pairedEnd
        );
        CUERR;

        cudaEventRecord(batchData.events[0], firstStream);
        cudaStreamWaitEvent(secondStream, batchData.events[0], 0); CUERR;

        cudaMemcpyAsync(
            batchData.h_numCandidatesPerAnchor.data(),
            batchData.d_numCandidatesPerAnchor2.data(),
            sizeof(int) * batchData.numTasks,
            D2H,
            secondStream
        ); CUERR;

        cudaEventRecord(batchData.events[0], secondStream);

        //determine task ids with removed mates

        assert(batchData.d_anchorIndicesWithRemovedMates.data() != nullptr);
        assert(batchData.h_numAnchorsWithRemovedMates.data() != nullptr);

        cubstatus = cub::DeviceSelect::Flagged(
            cubtempstorage,
            cubBytes,
            thrust::make_counting_iterator(0),
            d_anchorFlags,
            batchData.d_anchorIndicesWithRemovedMates.data(),
            batchData.h_numAnchorsWithRemovedMates.data(),
            batchData.numTasks,
            firstStream
        );
        assert(cubstatus == cudaSuccess);

        //copy selected candidate ids

        assert(batchData.d_candidateReadIds2.data() != nullptr);
        assert(batchData.h_numCandidates.data() != nullptr);

        cubstatus = cub::DeviceSelect::Flagged(
            cubtempstorage,
            cubBytes,
            batchData.d_candidateReadIds.data(),
            d_shouldBeKept,
            batchData.d_candidateReadIds2.data(),
            batchData.h_numCandidates.data(),
            batchData.totalNumCandidates,
            firstStream
        );
        assert(cubstatus == cudaSuccess);

        cudaStreamSynchronize(firstStream); CUERR; //wait for h_numCandidates   and h_numAnchorsWithRemovedMates
        batchData.numTasksWithMateRemoved = *batchData.h_numAnchorsWithRemovedMates;
        batchData.totalNumCandidates = *batchData.h_numCandidates;

        cubAllocator->DeviceFree(d_shouldBeKept); CUERR;

        //std::cerr << "new numTasksWithMateRemoved = " << batchData.numTasksWithMateRemoved << ", totalNumCandidates = " << batchData.totalNumCandidates << "\n";

        if(batchData.numTasksWithMateRemoved > 0){

            //copy mate sequence data of removed mates

            std::size_t cubtempstream2bytes = 0;
            cubstatus = cub::DeviceSelect::Flagged(
                nullptr,
                cubtempstream2bytes,
                batchData.d_inputanchormatedata.data(),
                thrust::make_transform_iterator(
                    thrust::make_counting_iterator(0),
                    SequenceFlagMultiplier{d_anchorFlags, int(batchData.encodedSequencePitchInInts)}
                ),
                batchData.d_anchormatedata.data(),
                thrust::make_discard_iterator(),
                batchData.numTasks * batchData.encodedSequencePitchInInts,
                secondStream
            );
            assert(cubstatus == cudaSuccess);
    
            void* cubtempstream2 = nullptr; cubAllocator->DeviceAllocate((void**)&cubtempstream2, cubtempstream2bytes, secondStream);
                
            assert(batchData.d_anchormatedata.data() != nullptr);

            cubstatus = cub::DeviceSelect::Flagged(
                cubtempstream2,
                cubtempstream2bytes,
                batchData.d_inputanchormatedata.data(),
                thrust::make_transform_iterator(
                    thrust::make_counting_iterator(0),
                    SequenceFlagMultiplier{d_anchorFlags, int(batchData.encodedSequencePitchInInts)}
                ),
                batchData.d_anchormatedata.data(),
                thrust::make_discard_iterator(),
                batchData.numTasks * batchData.encodedSequencePitchInInts,
                secondStream
            );
            assert(cubstatus == cudaSuccess);

            cubAllocator->DeviceFree(cubtempstream2);
        }

        cudaEventSynchronize(batchData.events[0]); CUERR;
        cubAllocator->DeviceFree(d_anchorFlags); CUERR;

        for(int i = 0, sum = 0; i < batchData.numTasks; i++){
            std::fill(
                batchData.h_segmentIdsOfReadIds.data() + sum,
                batchData.h_segmentIdsOfReadIds.data() + sum + batchData.h_numCandidatesPerAnchor[i],
                i
            );
            sum += batchData.h_numCandidatesPerAnchor[i];
        }

        cudaMemcpyAsync(
            batchData.d_anchorIndicesOfCandidates.data(),
            batchData.h_segmentIdsOfReadIds.data(),
            sizeof(int) * batchData.totalNumCandidates,
            H2D,
            firstStream
        ); CUERR;

        

        // //compute prefix sum of number of candidates per anchor
        // helpers::call_set_kernel_async(batchData.d_numCandidatesPerAnchorPrefixSum2.data(), 0, 0, firstStream);

        // cubstatus = cub::DeviceScan::InclusiveSum(
        //     cubtempstorage, 
        //     cubBytes, 
        //     batchData.d_numCandidatesPerAnchor2.data(), 
        //     batchData.d_numCandidatesPerAnchorPrefixSum2.data() + 1, 
        //     batchData.numTasks,
        //     firstStream
        // );
        // assert(cubstatus == cudaSuccess);

        // //compute segment ids for candidate read ids

        // helpers::call_fill_kernel_async(batchData.d_anchorIndicesOfCandidates.data(), batchData.totalNumCandidates, 0, firstStream);

        // setFirstSegmentIdsKernel<<<SDIV(batchData.numTasks, 256), 256, 0, firstStream>>>(
        //     batchData.d_numCandidatesPerAnchor2.data(),
        //     batchData.d_anchorIndicesOfCandidates.data(),
        //     batchData.d_numCandidatesPerAnchorPrefixSum2.data(),
        //     batchData.numTasks
        // );

        // cubstatus = cub::DeviceScan::InclusiveScan(
        //     cubtempstorage, 
        //     cubBytes, 
        //     batchData.d_anchorIndicesOfCandidates.data(), 
        //     batchData.d_anchorIndicesOfCandidates.data(), 
        //     cub::Max{},
        //     batchData.totalNumCandidates,
        //     firstStream
        // );
        // assert(cubstatus == cudaSuccess);
   

        ThrustCachingAllocator<char> thrustCachingAllocator1(deviceId, cubAllocator, firstStream);

        //cudaStreamWaitEvent(firstStream, batchData.events[0], 0); CUERR;

        
        //compute segmented set difference between candidate read ids and used candidate read ids
        auto d_candidateReadIds_end = GpuSegmentedSetOperation{}.difference(
            thrustCachingAllocator1,
            batchData.d_candidateReadIds2.data(),
            batchData.d_numCandidatesPerAnchor2.data(),
            batchData.d_numCandidatesPerAnchorPrefixSum2.data(),
            batchData.d_anchorIndicesOfCandidates.data(),
            batchData.totalNumCandidates,
            batchData.d_usedReadIds.data(),
            batchData.d_numUsedReadIdsPerAnchor.data(),
            batchData.d_numUsedReadIdsPerAnchorPrefixSum.data(),
            batchData.d_segmentIdsOfUsedReadIds.data(),
            batchData.totalNumberOfUsedIds,
            batchData.numTasks,        
            batchData.d_candidateReadIds.data(),
            batchData.d_numCandidatesPerAnchor.data(),
            batchData.d_anchorIndicesOfCandidates2.data(),
            firstStream
        );

        std::swap(batchData.d_anchorIndicesOfCandidates, batchData.d_anchorIndicesOfCandidates2);

        batchData.totalNumCandidates = std::distance(batchData.d_candidateReadIds.data(), d_candidateReadIds_end);

        //compute prefix sum of new segment sizes
    
        cubstatus = cub::DeviceScan::InclusiveSum(
            cubtempstorage, 
            cubBytes, 
            batchData.d_numCandidatesPerAnchor.data(), 
            batchData.d_numCandidatesPerAnchorPrefixSum.data() + 1, 
            batchData.numTasks,
            firstStream
        );
        assert(cubstatus == cudaSuccess);

        if(batchData.numTasksWithMateRemoved > 0){
            cudaEventRecord(batchData.events[0], secondStream);
            cudaStreamWaitEvent(firstStream, batchData.events[0], 0); CUERR;
        }

        cubAllocator->DeviceFree(cubtempstorage);
    }

    void loadCandidateSequenceData(BatchData& batchData, cudaStream_t stream) const{

        const int totalNumCandidates = batchData.totalNumCandidates;

        batchData.d_candidateSequencesLength.resize(batchData.totalNumCandidates);
        batchData.d_candidateSequencesData.resize(batchData.encodedSequencePitchInInts * batchData.totalNumCandidates);

        gpuReadStorage->gatherSequences(
            readStorageHandle,
            batchData.d_candidateSequencesData.get(),
            batchData.encodedSequencePitchInInts,
            batchData.h_candidateReadIds.get(),
            batchData.d_candidateReadIds.get(), //device accessible
            totalNumCandidates,
            stream
        );

        gpuReadStorage->gatherSequenceLengths(
            readStorageHandle,
            batchData.d_candidateSequencesLength.get(),
            batchData.d_candidateReadIds.get(),
            totalNumCandidates,
            stream
        );
    }

    void eraseDataOfRemovedMates(BatchData& batchData, cudaStream_t stream) const{

        const int totalNumCandidates = batchData.totalNumCandidates;

        int* d_intbuffercandidates = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_intbuffercandidates, sizeof(int) * batchData.totalNumCandidates, stream); CUERR;

        batchData.d_candidateSequencesLength2.resize(batchData.totalNumCandidates);
        batchData.d_candidateSequencesData2.resize(batchData.encodedSequencePitchInInts * batchData.totalNumCandidates);
        batchData.d_candidateReadIds2.resize(batchData.totalNumCandidates);

        constexpr int groupsize = 32;
        constexpr int blocksize = 128;
        constexpr int groupsperblock = blocksize / groupsize;
        dim3 block(blocksize,1,1);
        dim3 grid(SDIV(batchData.numTasksWithMateRemoved * groupsize, blocksize), 1, 1);
        const std::size_t smembytes = sizeof(unsigned int) * groupsperblock * batchData.encodedSequencePitchInInts;

        bool* d_keepflags = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_keepflags, sizeof(bool) * batchData.totalNumCandidates, stream); CUERR;

        helpers::call_fill_kernel_async(d_keepflags, totalNumCandidates, true, stream);

        readextendergpukernels::filtermatekernel<blocksize,groupsize><<<grid, block, smembytes, stream>>>(
            batchData.d_anchormatedata.data(),
            batchData.d_candidateSequencesData.data(),
            batchData.encodedSequencePitchInInts,
            batchData.d_numCandidatesPerAnchor.data(),
            batchData.d_numCandidatesPerAnchorPrefixSum.data(),
            batchData.d_anchorIndicesWithRemovedMates.data(),
            batchData.numTasksWithMateRemoved,
            d_keepflags
        ); CUERR;


        std::size_t requiredCubSize = 0;
        std::size_t requiredCubSize1 = 0;
        std::size_t requiredCubSize2 = 0;
        cudaError_t cubstatus = cub::DeviceScan::ExclusiveSum(
            nullptr,
            requiredCubSize1,
            d_keepflags, 
            d_intbuffercandidates, 
            totalNumCandidates, 
            stream
        );
        assert(cudaSuccess == cubstatus);

        cubstatus = cub::DeviceScan::InclusiveSum(
            nullptr,
            requiredCubSize2,
            batchData.d_numCandidatesPerAnchor.data(), 
            batchData.d_numCandidatesPerAnchorPrefixSum.data() + 1, 
            batchData.numTasks, 
            stream
        );
        assert(cudaSuccess == cubstatus);

        requiredCubSize = std::max(requiredCubSize1, requiredCubSize2);

        void* cubtemp; cubAllocator->DeviceAllocate((void**)&cubtemp, requiredCubSize, stream);

        cubstatus = cub::DeviceScan::ExclusiveSum(
            cubtemp,
            requiredCubSize,
            d_keepflags, 
            d_intbuffercandidates, 
            totalNumCandidates, 
            stream
        );
        assert(cudaSuccess == cubstatus);

        batchData.d_isPairedCandidate2.resize(batchData.d_isPairedCandidate.size());

        helpers::lambda_kernel<<<4096, 128, 0, stream>>>(
            [
                numTasks = batchData.numTasks,
                encodedSequencePitchInInts = batchData.encodedSequencePitchInInts,
                d_numCandidatesPerAnchor = batchData.d_numCandidatesPerAnchor.data(),
                d_numCandidatesPerAnchorPrefixSum = batchData.d_numCandidatesPerAnchorPrefixSum.data(),
                d_keepflags,
                d_outputpositions = d_intbuffercandidates,
                d_candidateReadIds = batchData.d_candidateReadIds.data(),
                d_candidateSequencesLength = batchData.d_candidateSequencesLength.data(),
                d_candidateSequencesData = batchData.d_candidateSequencesData.data(),
                d_anchorIndicesOfCandidates = batchData.d_anchorIndicesOfCandidates.data(),
                d_isPairedCandidate = batchData.d_isPairedCandidate.data(),
                d_candidateReadIdsOut = batchData.d_candidateReadIds2.data(),
                d_candidateSequencesLengthOut = batchData.d_candidateSequencesLength2.data(),
                d_candidateSequencesDataOut = batchData.d_candidateSequencesData2.data(),
                d_anchorIndicesOfCandidatesOut = batchData.d_anchorIndicesOfCandidates2.data(),
                d_isPairedCandidateOut = batchData.d_isPairedCandidate2.data()
            ] __device__ (){

                constexpr int elementsPerIteration = 128;

                using BlockReduce = cub::BlockReduce<int, elementsPerIteration>;
                __shared__ typename BlockReduce::TempStorage temp_storage;

                for(int t = blockIdx.x; t < numTasks; t += gridDim.x){
                    const int numCandidates = d_numCandidatesPerAnchor[t];
                    const int inputOffset = d_numCandidatesPerAnchorPrefixSum[t];

                    int numSelected = 0;

                    for(int i = threadIdx.x; i < numCandidates; i += blockDim.x){
                        if(d_keepflags[inputOffset + i]){
                            const int outputLocation = d_outputpositions[inputOffset + i];

                            d_candidateReadIdsOut[outputLocation] = d_candidateReadIds[inputOffset + i];
                            d_candidateSequencesLengthOut[outputLocation] = d_candidateSequencesLength[inputOffset + i];
                            d_anchorIndicesOfCandidatesOut[outputLocation] = d_anchorIndicesOfCandidates[inputOffset + i];
                            d_isPairedCandidateOut[outputLocation] = d_isPairedCandidate[inputOffset + i];

                            numSelected++;
                        }
                    }

                    for(int i = threadIdx.x; i < numCandidates * encodedSequencePitchInInts; i += blockDim.x){
                        const int which = i / encodedSequencePitchInInts;
                        const int what = i % encodedSequencePitchInInts;

                        if(d_keepflags[inputOffset + which]){
                            d_candidateSequencesDataOut[d_outputpositions[inputOffset + which] * encodedSequencePitchInInts + what] = d_candidateSequencesData[(inputOffset + which) * encodedSequencePitchInInts + what];
                        }
                    }

                    numSelected = BlockReduce(temp_storage).Sum(numSelected);
                    __syncthreads();
                    
                    if(threadIdx.x == 0){
                        if(numSelected != numCandidates){
                            assert(numSelected < numCandidates);
                            d_numCandidatesPerAnchor[t] = numSelected;
                            //printf("task %d, removed %d\n", t, numCandidates - numSelected);
                        }
                    }

                }
            }
        ); CUERR;

        cubAllocator->DeviceFree(d_intbuffercandidates); CUERR;
        cubAllocator->DeviceFree(d_keepflags); CUERR;

        //update prefix sum
        
        cubstatus = cub::DeviceScan::InclusiveSum(
            cubtemp,
            requiredCubSize,
            batchData.d_numCandidatesPerAnchor.data(), 
            batchData.d_numCandidatesPerAnchorPrefixSum.data() + 1, 
            batchData.numTasks, 
            stream
        );
        assert(cudaSuccess == cubstatus);       

        cubAllocator->DeviceFree(cubtemp);

        std::swap(batchData.d_candidateReadIds2, batchData.d_candidateReadIds);
        std::swap(batchData.d_candidateSequencesLength2, batchData.d_candidateSequencesLength);
        std::swap(batchData.d_candidateSequencesData2, batchData.d_candidateSequencesData);
        std::swap(batchData.d_anchorIndicesOfCandidates2, batchData.d_anchorIndicesOfCandidates);
        std::swap(batchData.d_isPairedCandidate2, batchData.d_isPairedCandidate);

        cudaMemcpyAsync(
            &batchData.totalNumCandidates,
            batchData.d_numCandidatesPerAnchorPrefixSum.data() + batchData.numTasks,
            sizeof(int),
            D2H,
            stream
        ); CUERR;

        cudaStreamSynchronize(stream); CUERR;
       
    }

    void calculateAlignments(BatchData& batchData, cudaStream_t stream) const{

        // cudaStreamSynchronize(stream); CUERR; //DEBUG
        // std::vector<int> vec1(batchData.d_numCandidatesPerAnchor.size());
        // cudaMemcpyAsync(vec1.data(), batchData.d_numCandidatesPerAnchor.data(), batchData.d_numCandidatesPerAnchor.sizeInBytes(), D2H, stream); CUERR;
        // cudaStreamSynchronize(stream); CUERR; //DEBUG
        // std::cerr << "Iteration: " << batchData.tasks[0].iteration << "\n";
        // for(int i = 0; i < batchData.numTasks; i++){
        //     std::cerr << vec1[i] << " ";
        // }
        // std::cerr << "\n";

        batchData.d_alignment_overlaps.resize(batchData.totalNumCandidates);
        batchData.d_alignment_shifts.resize(batchData.totalNumCandidates);
        batchData.d_alignment_nOps.resize(batchData.totalNumCandidates);
        batchData.d_alignment_best_alignment_flags.resize(batchData.totalNumCandidates);

        bool* d_alignment_isValid = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_alignment_isValid, sizeof(bool) * batchData.totalNumCandidates, stream); CUERR;
        
        batchData.h_numAnchors[0] = batchData.numTasks;

        const bool* const d_anchorContainsN = nullptr;
        const bool* const d_candidateContainsN = nullptr;
        const bool removeAmbiguousAnchors = false;
        const bool removeAmbiguousCandidates = false;
        const int maxNumAnchors = batchData.numTasks;
        const int maxNumCandidates = batchData.totalNumCandidates; //this does not need to be exact, but it must be >= batchData.d_numCandidatesPerAnchorPrefixSum[batchData.numTasks]
        const int maximumSequenceLength = batchData.encodedSequencePitchInInts * 16;
        const int encodedSequencePitchInInts2Bit = batchData.encodedSequencePitchInInts;
        const int min_overlap = goodAlignmentProperties->min_overlap;
        const float maxErrorRate = goodAlignmentProperties->maxErrorRate;
        const float min_overlap_ratio = goodAlignmentProperties->min_overlap_ratio;
        const float estimatedNucleotideErrorRate = correctionOptions->estimatedErrorrate;

        auto callAlignmentKernel = [&](void* d_tempstorage, size_t& tempstoragebytes){

            call_popcount_rightshifted_hamming_distance_kernel_async(
                d_tempstorage,
                tempstoragebytes,
                batchData.d_alignment_overlaps.get(),
                batchData.d_alignment_shifts.get(),
                batchData.d_alignment_nOps.get(),
                d_alignment_isValid,
                batchData.d_alignment_best_alignment_flags.get(),
                batchData.d_subjectSequencesData.get(),
                batchData.d_candidateSequencesData.get(),
                batchData.d_anchorSequencesLength.get(),
                batchData.d_candidateSequencesLength.get(),
                batchData.d_numCandidatesPerAnchorPrefixSum.get(),
                batchData.d_numCandidatesPerAnchor.get(),
                batchData.d_anchorIndicesOfCandidates.get(),
                batchData.h_numAnchors.get(),
                &batchData.d_numCandidatesPerAnchorPrefixSum[batchData.numTasks],
                d_anchorContainsN,
                removeAmbiguousAnchors,
                d_candidateContainsN,
                removeAmbiguousCandidates,
                maxNumAnchors,
                maxNumCandidates,
                maximumSequenceLength,
                encodedSequencePitchInInts2Bit,
                min_overlap,
                maxErrorRate,
                min_overlap_ratio,
                estimatedNucleotideErrorRate,
                stream,
                kernelLaunchHandle
            );
        };

        size_t tempstoragebytes = 0;
        callAlignmentKernel(nullptr, tempstoragebytes);

        void* d_tempstorage = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_tempstorage, tempstoragebytes, stream); CUERR;

        callAlignmentKernel(d_tempstorage, tempstoragebytes);

        cubAllocator->DeviceFree(d_tempstorage); CUERR;

        cubAllocator->DeviceFree(d_alignment_isValid); CUERR;
    }

    void filterAlignments(BatchData& batchData, cudaStream_t stream) const{

        const int totalNumCandidates = batchData.totalNumCandidates;
        const int numAnchors = batchData.numTasks;

        batchData.d_alignment_overlaps2.resize(batchData.totalNumCandidates);
        batchData.d_alignment_shifts2.resize(batchData.totalNumCandidates);
        batchData.d_alignment_nOps2.resize(batchData.totalNumCandidates);
        batchData.d_alignment_best_alignment_flags2.resize(batchData.totalNumCandidates);
        batchData.d_candidateSequencesLength2.resize(batchData.totalNumCandidates);
        batchData.d_candidateSequencesData2.resize(batchData.encodedSequencePitchInInts * batchData.totalNumCandidates);
        batchData.d_candidateReadIds2.resize(batchData.totalNumCandidates);

        bool* d_keepflags = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_keepflags, sizeof(bool) * batchData.totalNumCandidates, stream); CUERR; 

        helpers::call_fill_kernel_async(d_keepflags, batchData.totalNumCandidates, true, stream);


        dim3 block(128,1,1);
        dim3 grid(numAnchors, 1, 1);

        #if 0
            //filter alignments of candidates. d_keepflags[i] will be set to false if candidate[i] should be removed
        //batchData.d_numCandidatesPerAnchor2[i] contains new number of candidates for anchor i
        helpers::lambda_kernel<<<grid, block, 0, stream>>>(
            [
                d_alignment_best_alignment_flags = batchData.d_alignment_best_alignment_flags.data(),
                d_alignment_shifts = batchData.d_alignment_shifts.data(),
                d_alignment_overlaps = batchData.d_alignment_overlaps.data(),
                d_alignment_nOps = batchData.d_alignment_nOps.data(),
                d_anchorSequencesLength = batchData.d_anchorSequencesLength.data(),
                d_numCandidatesPerAnchor = batchData.d_numCandidatesPerAnchor.data(),
                d_numCandidatesPerAnchor2 = batchData.d_numCandidatesPerAnchor2.data(),
                d_numCandidatesPerAnchorPrefixSum = batchData.d_numCandidatesPerAnchorPrefixSum.data(),
                d_isPairedCandidate = batchData.d_isPairedCandidate.data(),
                d_keepflags,
                min_overlap_ratio = goodAlignmentProperties->min_overlap_ratio,
                numAnchors
            ] __device__ (){

                using BlockReduceFloat = cub::BlockReduce<float, 128>;
                using BlockReduceInt = cub::BlockReduce<int, 128>;

                __shared__ union {
                    typename BlockReduceFloat::TempStorage floatreduce;
                    typename BlockReduceInt::TempStorage intreduce;
                } cubtemp;

                __shared__ int intbroadcast;
                __shared__ float floatbroadcast;

                for(int a = blockIdx.x; a < numAnchors; a += gridDim.x){
                    const int num = d_numCandidatesPerAnchor[a];
                    const int offset = d_numCandidatesPerAnchorPrefixSum[a];
                    const float anchorLength = d_anchorSequencesLength[a];
                    int removed = 0;

                    //loop over candidates to compute relative overlap threshold

                    for(int c = threadIdx.x; c < num; c += blockDim.x){
                        
                        const auto alignmentflag = d_alignment_best_alignment_flags[offset + c];
                        const int shift = d_alignment_shifts[offset + c];

                        if(alignmentflag != BestAlignment_t::None && shift >= 0){
                            if(d_isPairedCandidate[offset+c]){
                                d_keepflags[offset + c] = true; //paired candidates always pass
                            }else{
                                const float overlap = d_alignment_overlaps[offset + c];
                                const float numMismatches = d_alignment_nOps[offset + c];                          
                                const float relativeOverlap = overlap / anchorLength;
                                const float errorrate = numMismatches / overlap;

                                if(fgeq(errorrate, 0.06f)){
                                    d_keepflags[offset + c] = true;
                                }else{
                                    d_keepflags[offset + c] = false;
                                    removed++;
                                }
                            }
                        }else{
                            //remove alignment with negative shift or bad alignments
                            d_keepflags[offset + c] = false;
                            removed++;
                        }                  
                    }

                    removed = BlockReduceInt(cubtemp.intreduce).Sum(removed);

                    if(threadIdx.x == 0){
                        d_numCandidatesPerAnchor2[a] = num - removed;
                    }
                    __syncthreads();
                }
            }
        ); CUERR;
        #else

        //filter alignments of candidates. d_keepflags[i] will be set to false if candidate[i] should be removed
        //batchData.d_numCandidatesPerAnchor2[i] contains new number of candidates for anchor i
        helpers::lambda_kernel<<<grid, block, 0, stream>>>(
            [
                d_alignment_best_alignment_flags = batchData.d_alignment_best_alignment_flags.data(),
                d_alignment_shifts = batchData.d_alignment_shifts.data(),
                d_alignment_overlaps = batchData.d_alignment_overlaps.data(),
                d_anchorSequencesLength = batchData.d_anchorSequencesLength.data(),
                d_numCandidatesPerAnchor = batchData.d_numCandidatesPerAnchor.data(),
                d_numCandidatesPerAnchor2 = batchData.d_numCandidatesPerAnchor2.data(),
                d_numCandidatesPerAnchorPrefixSum = batchData.d_numCandidatesPerAnchorPrefixSum.data(),
                d_isPairedCandidate = batchData.d_isPairedCandidate.data(),
                d_keepflags,
                min_overlap_ratio = goodAlignmentProperties->min_overlap_ratio,
                numAnchors
            ] __device__ (){

                using BlockReduceFloat = cub::BlockReduce<float, 128>;
                using BlockReduceInt = cub::BlockReduce<int, 128>;

                __shared__ union {
                    typename BlockReduceFloat::TempStorage floatreduce;
                    typename BlockReduceInt::TempStorage intreduce;
                } cubtemp;

                __shared__ int intbroadcast;
                __shared__ float floatbroadcast;

                for(int a = blockIdx.x; a < numAnchors; a += gridDim.x){
                    const int num = d_numCandidatesPerAnchor[a];
                    const int offset = d_numCandidatesPerAnchorPrefixSum[a];
                    const float anchorLength = d_anchorSequencesLength[a];
                    int removed = 0;

                    int threadReducedGoodAlignmentExists = 0;
                    float threadReducedRelativeOverlapThreshold = 0.0f;

                    //loop over candidates to compute relative overlap threshold

                    for(int c = threadIdx.x; c < num; c += blockDim.x){
                        const auto alignmentflag = d_alignment_best_alignment_flags[offset + c];
                        const int shift = d_alignment_shifts[offset + c];

                        if(alignmentflag != BestAlignment_t::None && shift >= 0){
                            if(!d_isPairedCandidate[offset+c]){
                                const float overlap = d_alignment_overlaps[offset + c];                            
                                const float relativeOverlap = overlap / anchorLength;
                                
                                if(relativeOverlap < 1.0f && fgeq(relativeOverlap, min_overlap_ratio)){
                                    threadReducedGoodAlignmentExists = 1;
                                    const float tmp = floorf(relativeOverlap * 10.0f) / 10.0f;
                                    threadReducedRelativeOverlapThreshold = fmaxf(threadReducedRelativeOverlapThreshold, tmp);
                                }
                            }
                        }else{
                            //remove alignment with negative shift or bad alignments
                            d_keepflags[offset + c] = false;
                            removed++;
                        }                       
                    }

                    int blockreducedGoodAlignmentExists = BlockReduceInt(cubtemp.intreduce)
                        .Sum(threadReducedGoodAlignmentExists);
                    if(threadIdx.x == 0){
                        intbroadcast = blockreducedGoodAlignmentExists;
                        //printf("task %d good: %d\n", a, blockreducedGoodAlignmentExists);
                    }
                    __syncthreads();

                    blockreducedGoodAlignmentExists = intbroadcast;

                    if(blockreducedGoodAlignmentExists > 0){
                        float blockreducedRelativeOverlapThreshold = BlockReduceFloat(cubtemp.floatreduce)
                            .Reduce(threadReducedRelativeOverlapThreshold, cub::Max());
                        if(threadIdx.x == 0){
                            floatbroadcast = blockreducedRelativeOverlapThreshold;
                            //printf("task %d thresh: %f\n", a, blockreducedRelativeOverlapThreshold);
                        }
                        __syncthreads();

                        blockreducedRelativeOverlapThreshold = floatbroadcast;

                        // loop over candidates and remove those with an alignment overlap threshold smaller than the computed threshold
                        for(int c = threadIdx.x; c < num; c += blockDim.x){
                            if(!d_isPairedCandidate[offset+c]){
                                if(d_keepflags[offset + c]){
                                    const float overlap = d_alignment_overlaps[offset + c];                            
                                    const float relativeOverlap = overlap / anchorLength;                 
        
                                    if(!fgeq(relativeOverlap, blockreducedRelativeOverlapThreshold)){
                                        d_keepflags[offset + c] = false;
                                        removed++;
                                    }
                                }
                            }
                        }
                    }else{
                        //NOOP.
                        //if no good alignment exists, no candidate is removed. we will try to work with the not-so-good alignments
                    }

                    removed = BlockReduceInt(cubtemp.intreduce).Sum(removed);

                    if(threadIdx.x == 0){
                        d_numCandidatesPerAnchor2[a] = num - removed;
                    }
                    __syncthreads();
                }
            }
        ); CUERR;
        #endif

        // cudaDeviceSynchronize(); CUERR; //DEBUG

        // std::vector<BestAlignment_t> vec3(batchData.d_alignment_best_alignment_flags.size());
        // std::vector<int> vec4(batchData.d_alignment_shifts.size());
        // std::vector<int> vec5(batchData.d_alignment_best_alignment_flags.size());

        // cudaMemcpyAsync(vec3.data(), batchData.d_alignment_best_alignment_flags.data(), batchData.d_alignment_best_alignment_flags.sizeInBytes(), D2H, stream); CUERR;
        // cudaMemcpyAsync(vec4.data(), batchData.d_alignment_shifts.data(), batchData.d_alignment_shifts.sizeInBytes(), D2H, stream); CUERR;
        // cudaMemcpyAsync(vec5.data(), batchData.d_alignment_overlaps.data(), batchData.d_alignment_overlaps.sizeInBytes(), D2H, stream); CUERR;

        // cudaDeviceSynchronize(); CUERR; //DEBUG

        // for(int k = 0; k < batchData.totalNumCandidates; k++){
        //     std::cerr << int(vec3[k]) << " " << vec4[k] << " " << vec5[k] << "\n";
        // }
        // std::cerr << "\n";

        // cudaStreamSynchronize(stream); CUERR; //DEBUG
        // std::vector<int> vec2(batchData.d_numCandidatesPerAnchor2.size());
        // cudaMemcpyAsync(vec2.data(), batchData.d_numCandidatesPerAnchor2.data(), batchData.d_numCandidatesPerAnchor2.sizeInBytes(), D2H, stream); CUERR;
        // cudaStreamSynchronize(stream); CUERR; //DEBUG
        // for(int i = 0; i < batchData.numTasks; i++){
        //     std::cerr << vec2[i] << " ";
        // }
        // std::cerr << "\n";

        // std::exit(0);


        //setup cub 
        auto d_zip_input = thrust::make_zip_iterator(
            thrust::make_tuple(
                batchData.d_alignment_nOps.data(),
                batchData.d_alignment_overlaps.data(),
                batchData.d_alignment_shifts.data(),
                batchData.d_alignment_best_alignment_flags.data(),
                batchData.d_candidateReadIds.data(),
                batchData.d_candidateSequencesLength.data(),
                batchData.d_isPairedCandidate.data()
            )
        );

        batchData.d_isPairedCandidate2.resize(batchData.d_isPairedCandidate.size());

        assert(batchData.d_alignment_nOps2.data() != nullptr);
        assert(batchData.d_alignment_overlaps2.data() != nullptr);
        assert(batchData.d_alignment_shifts2.data() != nullptr);
        assert(batchData.d_alignment_best_alignment_flags2.data() != nullptr);
        assert(batchData.d_candidateReadIds2.data() != nullptr);
        assert(batchData.d_candidateSequencesLength2.data() != nullptr);
        assert(batchData.d_isPairedCandidate2.data() != nullptr);

        auto d_zip_output = thrust::make_zip_iterator(
            thrust::make_tuple(
                batchData.d_alignment_nOps2.data(),
                batchData.d_alignment_overlaps2.data(),
                batchData.d_alignment_shifts2.data(),
                batchData.d_alignment_best_alignment_flags2.data(),
                batchData.d_candidateReadIds2.data(),
                batchData.d_candidateSequencesLength2.data(),
                batchData.d_isPairedCandidate2.data()
            )
        );

        std::size_t requiredCubSize1 = 0;
        cudaError_t cubstatus = cub::DeviceSelect::Flagged(
            nullptr, 
            requiredCubSize1, 
            d_zip_input, 
            d_keepflags, 
            d_zip_output, 
            batchData.d_numCandidates.data(), 
            totalNumCandidates, 
            stream
        );
        assert(cubstatus == cudaSuccess);

        std::size_t requiredCubSize2 = 0;
        cubstatus = cub::DeviceSelect::Flagged(
            nullptr,
            requiredCubSize2,
            batchData.d_candidateSequencesData.data(),
            thrust::make_transform_iterator(
                thrust::make_counting_iterator(0),
                SequenceFlagMultiplier{d_keepflags, int(batchData.encodedSequencePitchInInts)}
            ),
            batchData.d_candidateSequencesData2.data(),
            thrust::make_discard_iterator(),
            totalNumCandidates * batchData.encodedSequencePitchInInts,
            stream
        );
        assert(cubstatus == cudaSuccess);

        std::size_t requiredCubSize3 = 0;
        cubstatus = cub::DeviceScan::InclusiveSum(
            nullptr,
            requiredCubSize3,
            batchData.d_numCandidatesPerAnchor2.data(), 
            batchData.d_numCandidatesPerAnchorPrefixSum.data() + 1, 
            batchData.numTasks, 
            stream
        );
        assert(cudaSuccess == cubstatus);

        std::size_t requiredCubSize = std::max(std::max(requiredCubSize1, requiredCubSize2), requiredCubSize3);
        void* cubtemp; cubAllocator->DeviceAllocate((void**)&cubtemp, requiredCubSize, stream);

        //compact zip data
        assert(batchData.d_numCandidates.data() != nullptr);
        cubstatus = cub::DeviceSelect::Flagged(
            cubtemp, 
            requiredCubSize, 
            d_zip_input, 
            d_keepflags, 
            d_zip_output, 
            batchData.d_numCandidates.data(), 
            totalNumCandidates, 
            stream
        );
        assert(cubstatus == cudaSuccess);

        //compact sequence data.

        assert(batchData.d_candidateSequencesData2.data() != nullptr);
        cubstatus = cub::DeviceSelect::Flagged(
            cubtemp,
            requiredCubSize,
            batchData.d_candidateSequencesData.data(),
            thrust::make_transform_iterator(
                thrust::make_counting_iterator(0),
                SequenceFlagMultiplier{d_keepflags, int(batchData.encodedSequencePitchInInts)}
            ),
            batchData.d_candidateSequencesData2.data(),
            thrust::make_discard_iterator(),
            totalNumCandidates * batchData.encodedSequencePitchInInts,
            stream
        );
        assert(cubstatus == cudaSuccess);

        cubAllocator->DeviceFree(d_keepflags); CUERR;

        //update prefix sum
        cubstatus = cub::DeviceScan::InclusiveSum(
            cubtemp, 
            requiredCubSize, 
            batchData.d_numCandidatesPerAnchor2.data(), 
            batchData.d_numCandidatesPerAnchorPrefixSum.data() + 1, 
            batchData.numTasks, 
            stream
        );
        assert(cudaSuccess == cubstatus);


        cubAllocator->DeviceFree(cubtemp);

        std::swap(batchData.d_alignment_nOps2, batchData.d_alignment_nOps);
        std::swap(batchData.d_alignment_overlaps2, batchData.d_alignment_overlaps);
        std::swap(batchData.d_alignment_shifts2, batchData.d_alignment_shifts);
        std::swap(batchData.d_alignment_best_alignment_flags2, batchData.d_alignment_best_alignment_flags);
        std::swap(batchData.d_candidateReadIds2, batchData.d_candidateReadIds);
        std::swap(batchData.d_candidateSequencesLength2, batchData.d_candidateSequencesLength);
        std::swap(batchData.d_numCandidatesPerAnchor2, batchData.d_numCandidatesPerAnchor);
        std::swap(batchData.d_candidateSequencesData2, batchData.d_candidateSequencesData);
        std::swap(batchData.d_isPairedCandidate2, batchData.d_isPairedCandidate);

        cudaMemcpyAsync(
            &batchData.totalNumCandidates,
            batchData.d_numCandidatesPerAnchorPrefixSum.data() + batchData.numTasks,
            sizeof(int),
            D2H,
            stream
        ); CUERR;

        cudaStreamSynchronize(stream); CUERR;

        // cudaStreamSynchronize(stream); CUERR; //DEBUG
        // std::vector<int> vec1(batchData.d_numCandidatesPerAnchor.size());
        // cudaMemcpyAsync(vec1.data(), batchData.d_numCandidatesPerAnchor.data(), batchData.d_numCandidatesPerAnchor.sizeInBytes(), D2H, stream); CUERR;
        // cudaStreamSynchronize(stream); CUERR; //DEBUG
        // for(int i = 0; i < batchData.numTasks; i++){
        //     std::cerr << vec1[i] << " ";
        // }
        // std::cerr << "\n";
    }

    void loadCandidateQualityScores(BatchData& batchData, cudaStream_t stream, char* d_qualityscores) const{
        char* outputQualityScores = d_qualityscores;

        if(correctionOptions->useQualityScores){
            batchData.h_candidateReadIds.resize(batchData.totalNumCandidates);
            

            cudaMemcpyAsync(
                batchData.h_candidateReadIds.data(),
                batchData.d_candidateReadIds.data(),
                sizeof(read_number) * batchData.totalNumCandidates,
                D2H,
                stream
            ); CUERR;

            cudaStreamSynchronize(stream); CUERR;


            gpuReadStorage->gatherQualities(
                readStorageHandle,
                outputQualityScores,
                batchData.qualityPitchInBytes,
                batchData.h_candidateReadIds.data(),
                batchData.d_candidateReadIds.data(),
                batchData.totalNumCandidates,
                stream
            );

        }else{
            helpers::call_fill_kernel_async(
                outputQualityScores,
                batchData.qualityPitchInBytes * batchData.totalNumCandidates,
                'I',
                stream
            ); CUERR;
        }
        
    }


    void computeMSAs(BatchData& batchData, cudaStream_t firstStream, cudaStream_t secondStream) const{
        char* d_candidateQualityScores = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_candidateQualityScores, sizeof(char) * batchData.qualityPitchInBytes * batchData.totalNumCandidates, firstStream); CUERR;

        loadCandidateQualityScores(batchData, firstStream, d_candidateQualityScores);


        batchData.d_consensusEncoded.resize(batchData.numTasks * batchData.msaColumnPitchInElements);
        batchData.d_coverage.resize(batchData.numTasks * batchData.msaColumnPitchInElements);
        batchData.d_msa_column_properties.resize(batchData.numTasks);

        int* d_counts = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_counts, sizeof(int) * batchData.numTasks * 4 * batchData.msaColumnPitchInElements, firstStream); CUERR;

        float* d_weights = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_weights, sizeof(float) * batchData.numTasks * 4 * batchData.msaColumnPitchInElements, firstStream); CUERR;

        int* d_origCoverages = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_origCoverages, sizeof(int) * batchData.numTasks * batchData.msaColumnPitchInElements, firstStream); CUERR;

        float* d_origWeights = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_origWeights, sizeof(float) * batchData.numTasks * batchData.msaColumnPitchInElements, firstStream); CUERR;

        float* d_support = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_support, sizeof(float) * batchData.numTasks * batchData.msaColumnPitchInElements, firstStream); CUERR;

        batchData.d_consensusQuality.resize(batchData.numTasks * batchData.msaColumnPitchInElements);

        batchData.d_numCandidatesPerAnchor2.resize(batchData.numTasks);

        int* indices1 = nullptr; 
        int* indices2 = nullptr;
        cubAllocator->DeviceAllocate((void**)&indices1, sizeof(int) * batchData.totalNumCandidates, firstStream); CUERR;
        cubAllocator->DeviceAllocate((void**)&indices2, sizeof(int) * batchData.totalNumCandidates, firstStream); CUERR;

        

        helpers::lambda_kernel<<<batchData.numTasks, 128, 0, firstStream>>>(
            [
                indices1,
                d_numCandidatesPerAnchor = batchData.d_numCandidatesPerAnchor.get(),
                d_numCandidatesPerAnchorPrefixSum = batchData.d_numCandidatesPerAnchorPrefixSum.get()
            ] __device__ (){
                const int num = d_numCandidatesPerAnchor[blockIdx.x];
                const int offset = d_numCandidatesPerAnchorPrefixSum[blockIdx.x];
                
                for(int i = threadIdx.x; i < num; i += blockDim.x){
                    indices1[offset + i] = i;
                }
            }
        );

        gpu::GPUMultiMSA multiMSA;

        *batchData.h_numAnchors = batchData.numTasks;

        multiMSA.numMSAs = batchData.numTasks;
        multiMSA.columnPitchInElements = batchData.msaColumnPitchInElements;
        multiMSA.counts = d_counts;
        multiMSA.weights = d_weights;
        multiMSA.coverages = batchData.d_coverage.get();
        multiMSA.consensus = batchData.d_consensusEncoded.get();
        multiMSA.support = d_support;
        multiMSA.origWeights = d_origWeights;
        multiMSA.origCoverages = d_origCoverages;
        multiMSA.columnProperties = batchData.d_msa_column_properties.get();

        callConstructMultipleSequenceAlignmentsKernel_async(
            multiMSA,
            batchData.d_alignment_overlaps.get(),
            batchData.d_alignment_shifts.get(),
            batchData.d_alignment_nOps.get(),
            batchData.d_alignment_best_alignment_flags.get(),
            batchData.d_anchorSequencesLength.get(),
            batchData.d_candidateSequencesLength.get(),
            indices1, //d_indices,
            batchData.d_numCandidatesPerAnchor.get(),
            batchData.d_numCandidatesPerAnchorPrefixSum.get(),
            batchData.d_subjectSequencesData.get(),
            batchData.d_candidateSequencesData.get(),
            batchData.d_anchorQualityScores.get(), //d_anchor_qualities.get(),
            d_candidateQualityScores,
            batchData.h_numAnchors.get(), //d_numAnchors
            goodAlignmentProperties->maxErrorRate,
            batchData.numTasks,
            batchData.totalNumCandidates,
            true, //correctionOptions->useQualityScores,
            batchData.encodedSequencePitchInInts,
            batchData.qualityPitchInBytes,
            firstStream,
            kernelLaunchHandle
        );

        //refine msa
        bool* d_shouldBeKept = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_shouldBeKept, sizeof(bool) * batchData.totalNumCandidates, firstStream); CUERR;

        callMsaCandidateRefinementKernel_multiiter_async(
            indices2,
            batchData.d_numCandidatesPerAnchor2.data(),
            batchData.d_numCandidates2.get(),
            multiMSA,
            batchData.d_alignment_best_alignment_flags.get(),
            batchData.d_alignment_shifts.get(),
            batchData.d_alignment_nOps.get(),
            batchData.d_alignment_overlaps.get(),
            batchData.d_subjectSequencesData.get(),
            batchData.d_candidateSequencesData.get(),
            batchData.d_anchorSequencesLength.get(),
            batchData.d_candidateSequencesLength.get(),
            batchData.d_anchorQualityScores.get(), //d_anchor_qualities.get(),
            d_candidateQualityScores,
            d_shouldBeKept,
            batchData.d_numCandidatesPerAnchorPrefixSum.get(),
            batchData.h_numAnchors.get(),
            goodAlignmentProperties->maxErrorRate,
            batchData.numTasks,
            batchData.totalNumCandidates,
            true, //correctionOptions->useQualityScores,
            batchData.encodedSequencePitchInInts,
            batchData.qualityPitchInBytes,
            indices1, //d_indices,
            batchData.d_numCandidatesPerAnchor.get(),
            correctionOptions->estimatedCoverage,
            getNumRefinementIterations(),
            firstStream,
            kernelLaunchHandle
        );

        cubAllocator->DeviceFree(d_candidateQualityScores); CUERR;

        cudaEventRecord(batchData.events[0], firstStream); CUERR;
        cudaStreamWaitEvent(secondStream, batchData.events[0], 0); CUERR;

        cudaMemcpyAsync(
            batchData.h_numCandidates.data(),
            batchData.d_numCandidates.data(),
            sizeof(int),
            D2H,
            secondStream
        ); CUERR;

        cudaEventRecord(batchData.events[0], secondStream); CUERR;



        //compute quality of consensus
        helpers::lambda_kernel<<<batchData.numTasks, 256, 0, firstStream>>>(
            [
                consensusQuality = batchData.d_consensusQuality.data(),
                support = d_support,
                coverages = batchData.d_coverage.data(),
                msa_column_properties = batchData.d_msa_column_properties.data(),
                d_numCandidatesInMsa = batchData.d_numCandidatesPerAnchor2.data(),
                columnPitchInElements = batchData.msaColumnPitchInElements,
                numTasks = batchData.numTasks
            ] __device__ (){

                for(int t = blockIdx.x; t < numTasks; t += gridDim.x){
                    if(d_numCandidatesInMsa[t] > 0){
                        const float* const taskSupport = support + t * columnPitchInElements;
                        const int* const taskCoverage = coverages + t * columnPitchInElements;
                        char* const taskConsensusQuality = consensusQuality + t * columnPitchInElements;
                        const int begin = msa_column_properties[t].firstColumn_incl;
                        const int end = msa_column_properties[t].lastColumn_excl;

                        // if(threadIdx.x == 0){
                        //     printf("t %d, begin %d end %d\n", t, begin, end);
                        // }
                        // __syncthreads();

                        assert(begin >= 0);
                        assert(end < columnPitchInElements);

                        for(int i = begin + threadIdx.x; i < end; i += blockDim.x){
                            const float support = taskSupport[i];
                            const float cov = taskCoverage[i];

                            char q = getQualityChar(taskSupport[i]);

                            //scale down quality depending on coverage
                            q = char(float(q) * min(1.0f, cov * 1.0f / 5.0f));

                            taskConsensusQuality[i] = getQualityChar(support);
                        }
                    }
                }
            }
        ); CUERR;

        cubAllocator->DeviceFree(d_counts); CUERR;
        cubAllocator->DeviceFree(d_weights); CUERR;
        cubAllocator->DeviceFree(d_origCoverages); CUERR;
        cubAllocator->DeviceFree(d_origWeights); CUERR;
        cubAllocator->DeviceFree(d_support); CUERR;        
        cubAllocator->DeviceFree(indices1); CUERR;
        cubAllocator->DeviceFree(indices2); CUERR;
        cubAllocator->DeviceFree(d_shouldBeKept); CUERR;

        std::size_t cubBytes = 0;
        void* cubtemp = nullptr;

        cudaError_t cubstatus = cub::DeviceScan::InclusiveSum(
            cubtemp,
            cubBytes,
            batchData.d_numCandidatesPerAnchor2.get(), 
            batchData.d_numCandidatesPerAnchorPrefixSum2.get() + 1, 
            batchData.numTasks, 
            firstStream
        );
        assert(cubstatus == cudaSuccess);

        cubAllocator->DeviceAllocate((void**)&cubtemp, cubBytes, firstStream);

        cubstatus = cub::DeviceScan::InclusiveSum(
            cubtemp,
            cubBytes,
            batchData.d_numCandidatesPerAnchor2.get(), 
            batchData.d_numCandidatesPerAnchorPrefixSum2.get() + 1, 
            batchData.numTasks, 
            firstStream
        );
        assert(cubstatus == cudaSuccess);
        cubAllocator->DeviceFree(cubtemp);


        cudaEventSynchronize(batchData.events[0]); CUERR; //wait for h_numCandidates

        //only information about number of candidates are kept. all other information about candidates is discarded
        batchData.totalNumCandidates = *batchData.h_numCandidates; 

        std::swap(batchData.d_numCandidatesPerAnchor, batchData.d_numCandidatesPerAnchor2);
        std::swap(batchData.d_numCandidatesPerAnchorPrefixSum, batchData.d_numCandidatesPerAnchorPrefixSum2);  
        

    }

    void computeExtendedSequencesFromMSAs(BatchData& batchData, cudaStream_t stream) const{
        batchData.outputAnchorPitchInBytes = SDIV(batchData.decodedSequencePitchInBytes, 128) * 128;
        batchData.outputAnchorQualityPitchInBytes = SDIV(batchData.qualityPitchInBytes, 128) * 128;
        batchData.decodedMatesRevCPitchInBytes = SDIV(batchData.decodedSequencePitchInBytes, 128) * 128;

        batchData.h_accumExtensionsLengths.resize(batchData.numTasks);
        batchData.h_inputMateLengths.resize(batchData.numTasks);
        batchData.h_abortReasons.resize(batchData.numTasks);
        batchData.h_outputAnchors.resize(batchData.numTasks * batchData.outputAnchorPitchInBytes);
        batchData.h_outputAnchorQualities.resize(batchData.numTasks * batchData.outputAnchorQualityPitchInBytes);
        batchData.h_outputAnchorLengths.resize(batchData.numTasks);
        batchData.h_isPairedTask.resize(batchData.numTasks);
        batchData.h_decodedMatesRevC.resize(batchData.numTasks * batchData.decodedMatesRevCPitchInBytes);
        batchData.h_outputMateHasBeenFound.resize(batchData.numTasks);
        batchData.h_sizeOfGapToMate.resize(batchData.numTasks);

        batchData.h_scatterMap.resize(batchData.numTasks);

        int* d_accumExtensionsLengths = nullptr;
        int* d_inputMateLengths = nullptr;
        AbortReason* d_abortReasons = nullptr;
        char* d_outputAnchors = nullptr;
        char* d_outputAnchorQualities = nullptr;
        int* d_outputAnchorLengths = nullptr;
        bool* d_isPairedTask = nullptr;
        char* d_decodedMatesRevC = nullptr;
        bool* d_outputMateHasBeenFound = nullptr;
        int* d_sizeOfGapToMate = nullptr;

        cubAllocator->DeviceAllocate((void**)&d_accumExtensionsLengths, sizeof(int) * batchData.numTasks, stream); CUERR;
        cubAllocator->DeviceAllocate((void**)&d_inputMateLengths, sizeof(int) * batchData.numTasks, stream); CUERR;
        cubAllocator->DeviceAllocate((void**)&d_abortReasons, sizeof(AbortReason) * batchData.numTasks, stream); CUERR;
        cubAllocator->DeviceAllocate((void**)&d_outputAnchors, sizeof(char) * batchData.numTasks * batchData.outputAnchorPitchInBytes, stream); CUERR;
        cubAllocator->DeviceAllocate((void**)&d_outputAnchorQualities, sizeof(char) * batchData.numTasks * batchData.outputAnchorQualityPitchInBytes, stream); CUERR;
        cubAllocator->DeviceAllocate((void**)&d_outputAnchorLengths, sizeof(int) * batchData.numTasks, stream); CUERR;
        cubAllocator->DeviceAllocate((void**)&d_isPairedTask, sizeof(bool) * batchData.numTasks, stream); CUERR;
        cubAllocator->DeviceAllocate((void**)&d_decodedMatesRevC, sizeof(char) * batchData.numTasks * batchData.decodedMatesRevCPitchInBytes, stream); CUERR;
        cubAllocator->DeviceAllocate((void**)&d_outputMateHasBeenFound, sizeof(bool) * batchData.numTasks, stream); CUERR;
        cubAllocator->DeviceAllocate((void**)&d_sizeOfGapToMate, sizeof(int) * batchData.numTasks, stream); CUERR;



        helpers::call_fill_kernel_async(d_outputMateHasBeenFound, batchData.numTasks, false, stream); CUERR;
        helpers::call_fill_kernel_async(d_abortReasons, batchData.numTasks, AbortReason::None, stream); CUERR;

        // helpers::call_fill_kernel_async(
        //     thrust::make_zip_iterator(thrust::make_tuple(d_outputMateHasBeenFound, d_abortReasons)),
        //     batchData.numTasks,
        //     thrust::make_tuple(false, AbortReason::None),
        //     stream
        // );

        for(int i = 0; i < batchData.numTasks; i++){
            const int index = batchData.indicesOfActiveTasks[i];
            const auto& task = batchData.tasks[index];

            batchData.h_accumExtensionsLengths[i] = task.accumExtensionLengths;
            batchData.h_inputMateLengths[i] = task.mateLength;
            batchData.h_isPairedTask[i] = task.pairedEnd;
        }

        helpers::call_copy_n_kernel(
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.h_accumExtensionsLengths.data(),
                batchData.h_inputMateLengths.data(),
                batchData.h_isPairedTask.data()
            )),
            batchData.numTasks,
            thrust::make_zip_iterator(thrust::make_tuple(
                d_accumExtensionsLengths,
                d_inputMateLengths,
                d_isPairedTask
            )),
            stream
        );

        int numPairedEndTasks = 0;
        for(int i = 0; i < batchData.numTasks; i++){
            const int index = batchData.indicesOfActiveTasks[i];
            const auto& task = batchData.tasks[index];

            if(task.pairedEnd){

                // assert(task.decodedMateRevC.size() <= decodedMatesRevCPitchInBytes);
                // std::copy(task.decodedMateRevC.begin(), task.decodedMateRevC.end(), &h_decodedMatesRevC[i * decodedMatesRevCPitchInBytes]);
                std::copy(task.decodedMateRevC.begin(), task.decodedMateRevC.end(), &batchData.h_decodedMatesRevC[numPairedEndTasks * batchData.decodedMatesRevCPitchInBytes]);
                batchData.h_scatterMap[numPairedEndTasks] = i;
                numPairedEndTasks++;
            }
        }

        char* d_decodedMatesRevCDense = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_decodedMatesRevCDense, sizeof(char) * batchData.numTasks * batchData.decodedMatesRevCPitchInBytes, stream); CUERR;
        int* d_scatterMap = nullptr;
        cubAllocator->DeviceAllocate((void**)&d_scatterMap, sizeof(int) * batchData.numTasks, stream); CUERR;

        cudaMemcpyAsync(
            d_decodedMatesRevCDense,
            batchData.h_decodedMatesRevC.data(),
            sizeof(char) * batchData.decodedMatesRevCPitchInBytes * numPairedEndTasks,
            H2D,
            stream
        ); CUERR;

        cudaMemcpyAsync(
            d_scatterMap,
            batchData.h_scatterMap.data(),
            sizeof(int) * numPairedEndTasks,
            H2D,
            stream
        ); CUERR;

        helpers::lambda_kernel<<<batchData.numTasks, 128, 0, stream>>>(
            [
                numPairedEndTasks = numPairedEndTasks,
                decodedMatesRevCPitchInBytes = batchData.decodedMatesRevCPitchInBytes,
                d_scatterMap = d_scatterMap,
                d_decodedMatesRevCDense = d_decodedMatesRevCDense,
                d_decodedMatesRevC = d_decodedMatesRevC
            ] __device__ (){

                for(int t = blockIdx.x; t < numPairedEndTasks; t += gridDim.x){
                    const int destinationtask = d_scatterMap[t];

                    for(int i = threadIdx.x; i < decodedMatesRevCPitchInBytes; i += blockDim.x){
                        d_decodedMatesRevC[destinationtask * decodedMatesRevCPitchInBytes + i] = d_decodedMatesRevCDense[t * decodedMatesRevCPitchInBytes + i];
                    }
                }
            }
        ); CUERR;

        cubAllocator->DeviceFree(d_decodedMatesRevCDense); CUERR;
        cubAllocator->DeviceFree(d_scatterMap); CUERR;


           
        helpers::lambda_kernel<<<batchData.numTasks, 128, 0, stream>>>(
            [
                numTasks = batchData.numTasks,
                maxextensionPerStep = maxextensionPerStep,
                insertSize = insertSize,
                insertSizeStddev = insertSizeStddev,
                msaColumnPitchInElements = batchData.msaColumnPitchInElements,
                d_numCandidatesPerAnchor = batchData.d_numCandidatesPerAnchor.data(),
                d_msa_column_properties = batchData.d_msa_column_properties.data(),
                d_consensusEncoded = batchData.d_consensusEncoded.data(),
                d_consensusQuality = batchData.d_consensusQuality.data(),
                d_coverage = batchData.d_coverage.data(),
                d_anchorSequencesLength = batchData.d_anchorSequencesLength.data(),
                d_accumExtensionsLengths = (int*)d_accumExtensionsLengths,
                d_inputMateLengths = (int*)d_inputMateLengths,
                d_abortReasons = (AbortReason*)d_abortReasons,
                d_outputAnchors = (char*)d_outputAnchors,
                outputAnchorPitchInBytes = batchData.outputAnchorPitchInBytes,
                d_outputAnchorQualities = (char*)d_outputAnchorQualities,
                outputAnchorQualityPitchInBytes = batchData.outputAnchorQualityPitchInBytes,
                d_outputAnchorLengths = (int*)d_outputAnchorLengths,
                d_isPairedTask = (bool*)d_isPairedTask,
                d_decodedMatesRevC = (char*)d_decodedMatesRevC,
                decodedMatesRevCPitchInBytes = batchData.decodedMatesRevCPitchInBytes,
                d_outputMateHasBeenFound = (bool*)d_outputMateHasBeenFound,
                d_sizeOfGapToMate = (int*)d_sizeOfGapToMate
            ] __device__ (){

                auto decodeConsensus = [](const std::uint8_t encoded){
                    char decoded = 'F';
                    if(encoded == std::uint8_t{0}){
                        decoded = 'A';
                    }else if(encoded == std::uint8_t{1}){
                        decoded = 'C';
                    }else if(encoded == std::uint8_t{2}){
                        decoded = 'G';
                    }else if(encoded == std::uint8_t{3}){
                        decoded = 'T';
                    }
                    return decoded;
                };

                using BlockReduce = cub::BlockReduce<int, 128>;

                __shared__ union{
                    typename BlockReduce::TempStorage reduce;
                } temp;

                __shared__ int broadcastsmem_int;

                for(int t = blockIdx.x; t < numTasks; t += gridDim.x){
                    const int numCandidates = d_numCandidatesPerAnchor[t];

                    if(numCandidates > 0){
                        const auto msaProps = d_msa_column_properties[t];

                        assert(msaProps.firstColumn_incl == 0);
                        assert(msaProps.lastColumn_excl <= msaColumnPitchInElements);

                        const int anchorLength = d_anchorSequencesLength[t];
                        int accumExtensionsLength = d_accumExtensionsLengths[t];
                        const int mateLength = d_inputMateLengths[t];
                        const bool isPaired = d_isPairedTask[t];

                        const int consensusLength = msaProps.lastColumn_excl - msaProps.firstColumn_incl;

                        const std::uint8_t* const consensusEncoded = d_consensusEncoded + t * msaColumnPitchInElements;
                        auto consensusDecoded = thrust::transform_iterator(consensusEncoded, decodeConsensus);
                        const char* const consensusQuality = d_consensusQuality + t * msaColumnPitchInElements;
                        const int* const msacoverage = d_coverage + t * msaColumnPitchInElements;
                        const char* const decodedMateRevC = d_decodedMatesRevC + t * decodedMatesRevCPitchInBytes;

                        AbortReason* const abortReasonPtr = d_abortReasons + t;
                        char* const outputAnchor = d_outputAnchors + t * outputAnchorPitchInBytes;
                        char* const outputAnchorQuality = d_outputAnchorQualities + t * outputAnchorQualityPitchInBytes;
                        int* const outputAnchorLengthPtr = d_outputAnchorLengths + t;
                        bool* const mateHasBeenFoundPtr = d_outputMateHasBeenFound + t;

                        int extendBy = std::min(
                            consensusLength - anchorLength, 
                            maxextensionPerStep
                        );
                        //cannot extend over fragment 
                        extendBy = std::min(extendBy, (insertSize + insertSizeStddev - mateLength) - accumExtensionsLength);

                        constexpr int minCoverageForExtension = 3;

                        //auto firstLowCoverageIter = std::find_if(coverage + anchorLength, coverage + consensusLength, [&](int cov){ return cov < minCoverageForExtension; });
                        //coverage is monotonically decreasing. convert coverages to 1 if >= minCoverageForExtension, else 0. Find position of first 0
                        int myPos = consensusLength;
                        for(int i = anchorLength + threadIdx.x; i < consensusLength; i += blockDim.x){
                            int flag = msacoverage[i] < minCoverageForExtension ? 0 : 1;
                            if(flag == 0 && i < myPos){
                                myPos = i;
                            }
                        }

                        myPos = BlockReduce(temp.reduce).Reduce(myPos, cub::Min{});

                        if(threadIdx.x == 0){
                            broadcastsmem_int = myPos;
                        }
                        __syncthreads();
                        myPos = broadcastsmem_int;
                        __syncthreads();

                        extendBy = myPos - anchorLength;
                        extendBy = std::min(extendBy, (insertSize + insertSizeStddev - mateLength) - accumExtensionsLength);

                        auto makeAnchorForNextIteration = [&](){
                            if(extendBy == 0){
                                if(threadIdx.x == 0){
                                    *abortReasonPtr = AbortReason::MsaNotExtended;
                                }
                            }else{
                                accumExtensionsLength += extendBy;
                                if(threadIdx.x == 0){
                                    d_accumExtensionsLengths[t] = accumExtensionsLength;
                                    *outputAnchorLengthPtr = anchorLength;
                                }

                                for(int i = threadIdx.x; i < anchorLength; i += blockDim.x){
                                    outputAnchor[i] = consensusDecoded[extendBy + i];
                                    outputAnchorQuality[i] = consensusQuality[extendBy + i];
                                }
                            }
                        };

                        constexpr int requiredOverlapMate = 70; //TODO relative overlap 
                        constexpr float maxRelativeMismatchesInOverlap = 0.06f;
                        constexpr int maxAbsoluteMismatchesInOverlap = 10;

                        const int maxNumMismatches = std::min(int(mateLength * maxRelativeMismatchesInOverlap), maxAbsoluteMismatchesInOverlap);

                        

                        if(isPaired && accumExtensionsLength + consensusLength - requiredOverlapMate + mateLength >= insertSize - insertSizeStddev){
                            //for each possibility to overlap the mate and consensus such that the merged sequence would end in the desired range [insertSize - insertSizeStddev, insertSize + insertSizeStddev]

                            const int firstStartpos = std::max(0, insertSize - insertSizeStddev - accumExtensionsLength - mateLength);
                            const int lastStartposExcl = std::min(
                                std::max(0, insertSize + insertSizeStddev - accumExtensionsLength - mateLength) + 1,
                                consensusLength - requiredOverlapMate
                            );

                            int bestOverlapMismatches = std::numeric_limits<int>::max();
                            int bestOverlapStartpos = -1;

                            for(int startpos = firstStartpos; startpos < lastStartposExcl; startpos++){
                                //compute metrics of overlap

                                //Hamming distance. positions which do not overlap are not accounted for
                                int ham = 0;
                                for(int i = threadIdx.x; i < min(consensusLength - startpos, mateLength); i += blockDim.x){
                                    ham += (consensusDecoded[startpos + i] != decodedMateRevC[i]) ? 1 : 0;
                                }

                                ham = BlockReduce(temp.reduce).Sum(ham);

                                if(threadIdx.x == 0){
                                    broadcastsmem_int = ham;
                                }
                                __syncthreads();
                                ham = broadcastsmem_int;
                                __syncthreads();

                                if(bestOverlapMismatches > ham){
                                    bestOverlapMismatches = ham;
                                    bestOverlapStartpos = startpos;
                                }

                                if(bestOverlapMismatches == 0){
                                    break;
                                }
                            }

                            // if(threadIdx.x == 0){
                            //     printf("gpu: bestOverlapMismatches %d,bestOverlapStartpos %d\n", bestOverlapMismatches, bestOverlapStartpos);
                            // }

                            if(bestOverlapMismatches <= maxNumMismatches){
                                const int mateStartposInConsensus = bestOverlapStartpos;
                                const int missingPositionsBetweenAnchorEndAndMateBegin = std::max(0, mateStartposInConsensus - anchorLength);
                                // if(threadIdx.x == 0){
                                //     printf("missingPositionsBetweenAnchorEndAndMateBegin %d\n", missingPositionsBetweenAnchorEndAndMateBegin);
                                // }

                                if(missingPositionsBetweenAnchorEndAndMateBegin > 0){
                                    //bridge the gap between current anchor and mate

                                    for(int i = threadIdx.x; i < missingPositionsBetweenAnchorEndAndMateBegin; i += blockDim.x){
                                        outputAnchor[i] = consensusDecoded[anchorLength + i];
                                        outputAnchorQuality[i] = consensusQuality[anchorLength + i];
                                    }

                                    accumExtensionsLength += anchorLength;

                                    if(threadIdx.x == 0){
                                        d_accumExtensionsLengths[t] = accumExtensionsLength;
                                        *outputAnchorLengthPtr = missingPositionsBetweenAnchorEndAndMateBegin;
                                        *mateHasBeenFoundPtr = true;
                                        d_sizeOfGapToMate[t] = missingPositionsBetweenAnchorEndAndMateBegin;
                                    }
                                }else{
                                    accumExtensionsLength += mateStartposInConsensus;

                                    if(threadIdx.x == 0){
                                        d_accumExtensionsLengths[t] = accumExtensionsLength;
                                        *outputAnchorLengthPtr = 0;
                                        *mateHasBeenFoundPtr = true;
                                        d_sizeOfGapToMate[t] = 0;
                                    }
                                }

                                
                            }else{
                                makeAnchorForNextIteration();
                            }
                        }else{
                            makeAnchorForNextIteration();
                        }

                    }else{ //numCandidates == 0
                        d_abortReasons[t] = AbortReason::NoPairedCandidatesAfterAlignment;
                    }
                }
            }
        );

        helpers::call_copy_n_kernel(
            thrust::make_zip_iterator(thrust::make_tuple(
                d_accumExtensionsLengths,
                d_abortReasons,
                d_outputMateHasBeenFound,
                d_sizeOfGapToMate,
                d_outputAnchorLengths
            )),
            batchData.numTasks,
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.h_accumExtensionsLengths.data(),
                batchData.h_abortReasons.data(),
                batchData.h_outputMateHasBeenFound.data(),
                batchData.h_sizeOfGapToMate.data(),
                batchData.h_outputAnchorLengths.data()
            )),
            stream
        );

        cudaMemcpyAsync(
            batchData.h_outputAnchors.data(),
            d_outputAnchors,
            sizeof(char) * batchData.outputAnchorPitchInBytes * batchData.numTasks,
            D2H,
            stream
        ); CUERR;

        cudaMemcpyAsync(
            batchData.h_outputAnchorQualities.data(),
            d_outputAnchorQualities,
            sizeof(char) * batchData.outputAnchorQualityPitchInBytes * batchData.numTasks,
            D2H,
            stream
        ); CUERR;

        cubAllocator->DeviceFree(d_accumExtensionsLengths); CUERR;
        cubAllocator->DeviceFree(d_inputMateLengths); CUERR;
        cubAllocator->DeviceFree(d_abortReasons); CUERR;
        cubAllocator->DeviceFree(d_outputAnchors); CUERR;
        cubAllocator->DeviceFree(d_outputAnchorQualities); CUERR;
        cubAllocator->DeviceFree(d_outputAnchorLengths); CUERR;
        cubAllocator->DeviceFree(d_isPairedTask); CUERR;
        cubAllocator->DeviceFree(d_decodedMatesRevC); CUERR;
        cubAllocator->DeviceFree(d_outputMateHasBeenFound); CUERR;
        cubAllocator->DeviceFree(d_sizeOfGapToMate); CUERR;
    }
    
    
    
    void copyBuffersToHost(BatchData& batchData, cudaStream_t firstStream, cudaStream_t secondStream) const{
        batchData.h_candidateReadIds.resize(batchData.totalNumCandidates);
        batchData.h_numCandidatesPerAnchor.resize(batchData.numTasks);
        batchData.h_numCandidatesPerAnchorPrefixSum.resize(batchData.numTasks + 1);

        batchData.h_numCandidatesPerAnchorPrefixSum[0] = 0;
       
        helpers::call_copy_n_kernel(
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.d_numCandidatesPerAnchorPrefixSum.data() + 1,
                batchData.d_numCandidatesPerAnchor.data()
            )),
            batchData.numTasks,
            thrust::make_zip_iterator(thrust::make_tuple(
                batchData.h_numCandidatesPerAnchorPrefixSum.data() + 1,
                batchData.h_numCandidatesPerAnchor.data()
            )),
            firstStream
        );   

        cudaMemcpyAsync(
            batchData.h_candidateReadIds.data(),
            batchData.d_candidateReadIds.data(),
            sizeof(read_number) * batchData.totalNumCandidates,
            D2H,
            firstStream
        );
    }

};




}


#endif