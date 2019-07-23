#include <build.hpp>

#include <config.hpp>

#include <options.hpp>

#include <minhasher.hpp>
#include <minhasher_transform.hpp>
#include "readstorage.hpp"
#include "sequencefileio.hpp"
#include "sequence.hpp"
#include "threadsafe_buffer.hpp"

#include <stdexcept>
#include <iostream>
#include <limits>
#include <thread>
#include <future>
#include <mutex>
#include <iterator>
#include <random>
#include <omp.h>


#ifdef __NVCC__

namespace care{
namespace gpu{



    BuiltDataStructure<DistributedReadStorage> buildGpuReadStorage(const FileOptions& fileOptions,
                                                const RuntimeOptions& runtimeOptions,
                                                bool useQualityScores,
                                                read_number expectedNumberOfReads,
                                                int expectedMaximumReadLength){



        if(false && fileOptions.load_binary_reads_from != ""){
            BuiltDataStructure<DistributedReadStorage> result;
            auto& readStorage = result.data;

            readStorage.loadFromFile(fileOptions.load_binary_reads_from, runtimeOptions.deviceIds);
            result.builtType = BuiltType::Loaded;

            if(useQualityScores && !readStorage.canUseQualityScores())
                throw std::runtime_error("Quality scores are required but not present in compressed sequence file!");
            if(!useQualityScores && readStorage.canUseQualityScores())
                std::cerr << "Warning. The loaded compressed read file contains quality scores, but program does not use them!\n";

            std::cout << "Loaded binary reads from " << fileOptions.load_binary_reads_from << std::endl;

            return result;
        }else{
            //int nThreads = std::max(1, std::min(runtimeOptions.threads, 2));
            const int nThreads = std::max(1, runtimeOptions.threads);

            constexpr std::array<char, 4> bases = {'A', 'C', 'G', 'T'};
            int Ncount = 0;

            BuiltDataStructure<DistributedReadStorage> result;
            DistributedReadStorage& readstorage = result.data;

            readstorage = std::move(DistributedReadStorage{runtimeOptions.deviceIds, expectedNumberOfReads, useQualityScores, expectedMaximumReadLength});
            result.builtType = BuiltType::Constructed;

            constexpr size_t maxbuffersize = 1000000;

            auto flushBuffers = [&](std::vector<read_number>& indicesBuffer, std::vector<Read>& readsBuffer){
                if(indicesBuffer.size() > 0){
                    std::cerr << "flush\n";
                    readstorage.setReads(indicesBuffer, readsBuffer, nThreads);
                    indicesBuffer.clear();
                    readsBuffer.clear();
                }
            };

            auto handle_read = [&](std::uint64_t readIndex, Read& read, std::vector<read_number>& indicesBuffer, std::vector<Read>& readsBuffer){
                const int readLength = int(read.sequence.size());

                if(readIndex >= expectedNumberOfReads){
                    throw std::runtime_error("Error! Expected " + std::to_string(expectedNumberOfReads)
                                            + " reads, but file contains at least "
                                            + std::to_string(readIndex) + " reads.");
                }

                if(readLength > expectedMaximumReadLength){
                    throw std::runtime_error("Error! Expected maximum read length = "
                                            + std::to_string(expectedMaximumReadLength)
                                            + ", but read " + std::to_string(readIndex)
                                            + "has length " + std::to_string(readLength));
                }

                for(auto& c : read.sequence){
                    if(c == 'a') c = 'A';
                    if(c == 'c') c = 'C';
                    if(c == 'g') c = 'G';
                    if(c == 't') c = 'T';
                    if(c == 'N' || c == 'n'){
                        c = bases[Ncount];
                        Ncount = (Ncount + 1) % 4;
                    }
                }

                indicesBuffer.emplace_back(readIndex);
                readsBuffer.emplace_back(read);
                if(indicesBuffer.size() > maxbuffersize){
                    flushBuffers(indicesBuffer, readsBuffer);
                }
            };

            // if(nThreads == 1){
            //     std::unique_ptr<SequenceFileReader> reader = makeSequenceReader(fileOptions.inputfile, fileOptions.format);
            //
            //     Read read;
            //
            //     std::vector<read_number> indicesBuffer;
            //     std::vector<Read> readsBuffer;
            //     indicesBuffer.reserve(maxbuffersize);
            //     readsBuffer.reserve(maxbuffersize);
            //
            //     while (reader->getNextRead(&read)) {
            //         std::uint64_t readIndex = reader->getReadnum() - 1;
            //
            //         handle_read(readIndex, read, indicesBuffer, readsBuffer);
            //     }
            //
            //     if(indicesBuffer.size() > 0){
            //         flushBuffers(indicesBuffer, readsBuffer);
            //     }
            //
            // }else{

                using CommunicationBuffer_t = ThreadsafeBuffer<std::pair<Read, std::uint64_t>, 30000>;
                CommunicationBuffer_t commBuffer;

                auto inserterFunc = [&](){
                    std::vector<read_number> indicesBuffer;
                    std::vector<Read> readsBuffer;
                    indicesBuffer.reserve(maxbuffersize);
                    readsBuffer.reserve(maxbuffersize);

                    auto pair = commBuffer.get();

                    while (pair != commBuffer.defaultValue) {
                        Read& read = pair.first;
                        const std::uint64_t readIndex = pair.second;

                        handle_read(readIndex, read, indicesBuffer, readsBuffer);

                        pair = commBuffer.get();
                    }

                    if(indicesBuffer.size() > 0){
                        flushBuffers(indicesBuffer, readsBuffer);
                    }
                };

                auto inserterThread = std::async(std::launch::async, inserterFunc);

                std::unique_ptr<SequenceFileReader> reader = makeSequenceReader(fileOptions.inputfile, fileOptions.format);

                Read read;
                while (reader->getNextRead(&read)) {
                    std::uint64_t readnum = reader->getReadnum()-1;
                    commBuffer.add( { std::move(read), readnum });
                }

                commBuffer.done();

                inserterThread.wait();
            //}

            return result;
        }

    }


    BuiltDataStructure<Minhasher> build_minhasher(const FileOptions& fileOptions,
                                			   const RuntimeOptions& runtimeOptions,
                                			   std::uint64_t nReads,
                                               const MinhashOptions& minhashOptions,
                                			   const DistributedReadStorage& readStorage){

        BuiltDataStructure<Minhasher> result;
        auto& minhasher = result.data;

        auto identity = [](auto i){return i;};

        minhasher = std::move(Minhasher{minhashOptions});

        minhasher.init(nReads);

        if(fileOptions.load_hashtables_from != ""){
            minhasher.loadFromFile(fileOptions.load_hashtables_from);
            result.builtType = BuiltType::Loaded;

            std::cout << "Loaded hash tables from " << fileOptions.load_hashtables_from << std::endl;
        }else{
            result.builtType = BuiltType::Constructed;

            const int oldnumthreads = omp_get_thread_num();

            omp_set_num_threads(runtimeOptions.threads);
            constexpr int numMapsPerBatch = 16;
            constexpr read_number parallelReads = 10000000;

            const int numBatches = SDIV(minhashOptions.maps, numMapsPerBatch);

            for(int batch = 0; batch < numBatches; batch++){
                const int firstMap = batch * numMapsPerBatch;
                const int lastMap = std::min(minhashOptions.maps, (batch+1) * numMapsPerBatch);
                const int numMaps = lastMap - firstMap;
                std::vector<int> mapIds(numMaps);
                std::iota(mapIds.begin(), mapIds.end(), firstMap);

                for(auto mapId : mapIds){
                    minhasher.initMap(mapId);
                }

                read_number numReads = readStorage.getNumberOfReads();
                int numIters = SDIV(numReads, parallelReads);

                auto sequencehandle = readStorage.makeGatherHandleSequences();
                auto lengthhandle = readStorage.makeGatherHandleLengths();
                size_t sequencepitch = getEncodedNumInts2BitHiLo(readStorage.getSequenceLengthLimit()) * sizeof(int);

                for(int iter = 0; iter < numIters; iter++){
                    read_number readIdBegin = iter * parallelReads;
                    read_number readIdEnd = std::min((iter+1) * parallelReads, numReads);

                    std::vector<read_number> indices(readIdEnd - readIdBegin);
                    std::iota(indices.begin(), indices.end(), readIdBegin);

                    std::vector<char> sequenceData(indices.size() * sequencepitch);
                    std::vector<DistributedReadStorage::Length_t> lengths(indices.size());

                    auto future1 = readStorage.gatherSequenceDataToHostBufferAsync(
                                                sequencehandle,
                                                sequenceData.data(),
                                                sequencepitch,
                                                indices.data(),
                                                indices.size(),
                                                1);
                    auto future2 = readStorage.gatherSequenceLengthsToHostBufferAsync(
                                                lengthhandle,
                                                lengths.data(),
                                                indices.data(),
                                                indices.size(),
                                                1);

                    future1.wait();
                    future2.wait();

                    #pragma omp parallel for
                    for(read_number readId = readIdBegin; readId < readIdEnd; readId++){
                        read_number localId = readId - readIdBegin;
        				const char* encodedsequence = (const char*)&sequenceData[localId * sequencepitch];
        				const int sequencelength = lengths[localId];
        				std::string sequencestring = get2BitHiLoString((const unsigned int*)encodedsequence, sequencelength);
                        minhasher.insertSequence(sequencestring, readId, mapIds);
                    }
                }

                for(auto mapId : mapIds){
                    transform_minhasher(minhasher, mapId, runtimeOptions.deviceIds);
                }
            }

            omp_set_num_threads(oldnumthreads);
        }

        //TIMERSTARTCPU(finalize_hashtables);
        //minhasher.transform();
        //TIMERSTOPCPU(finalize_hashtables);

        return result;
    }



    BuiltGpuDataStructures buildGpuDataStructures(const MinhashOptions& minhashOptions,
                                			const CorrectionOptions& correctionOptions,
                                			const RuntimeOptions& runtimeOptions,
                                			const FileOptions& fileOptions){

        BuiltGpuDataStructures result;

        auto& sequenceFileProperties = result.sequenceFileProperties;

        if(true || fileOptions.load_binary_reads_from == "") {
            if(fileOptions.nReads == 0 || fileOptions.maximum_sequence_length == 0) {
                std::cout << "Scanning file to get number of reads and maximum sequence length." << std::endl;
                sequenceFileProperties = getSequenceFileProperties(fileOptions.inputfile, fileOptions.format);
            }else{
                sequenceFileProperties.maxSequenceLength = fileOptions.maximum_sequence_length;
                sequenceFileProperties.minSequenceLength = 0;
                sequenceFileProperties.nReads = fileOptions.nReads;
            }
        }

        TIMERSTARTCPU(build_readstorage);
        result.builtReadStorage = buildGpuReadStorage(fileOptions,
                                                  runtimeOptions,
                                                  correctionOptions.useQualityScores,
                                                  sequenceFileProperties.nReads,
                                                  sequenceFileProperties.maxSequenceLength);
        TIMERSTOPCPU(build_readstorage);

        const auto& readStorage = result.builtReadStorage.data;

        if(result.builtReadStorage.builtType == BuiltType::Loaded) {
            sequenceFileProperties.nReads = readStorage.getNumberOfReads();
            sequenceFileProperties.maxSequenceLength = readStorage.getStatistics().maximumSequenceLength;
            sequenceFileProperties.minSequenceLength = readStorage.getStatistics().minimumSequenceLength;
        }

        TIMERSTARTCPU(build_minhasher);
        result.builtMinhasher = build_minhasher(fileOptions, runtimeOptions, sequenceFileProperties.nReads, minhashOptions, readStorage);
        TIMERSTOPCPU(build_minhasher);

        return result;

    }

}
}


#endif
