#include <dispatch_care.hpp>
#include <gpu/gpuminhasher.cuh>

#include <config.hpp>
#include <options.hpp>
#include <readlibraryio.hpp>
#include <minhasher.hpp>
//#include <build.hpp>
#include <gpu/distributedreadstorage.hpp>
#include <gpu/correct_gpu.hpp>
#include <correctionresultprocessing.hpp>

#include <rangegenerator.hpp>

#include <gpu/readextension_gpu.hpp>
#include <extensionresultprocessing.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

#include <experimental/filesystem>

namespace filesys = std::experimental::filesystem;

namespace care{

    std::vector<int> getUsableDeviceIds(std::vector<int> deviceIds){
        int nDevices;

        cudaGetDeviceCount(&nDevices); CUERR;

        std::vector<int> invalidIds;

        for(int id : deviceIds) {
            if(id >= nDevices) {
                invalidIds.emplace_back(id);
                std::cout << "Found invalid device Id: " << id << std::endl;
            }
        }

        if(invalidIds.size() > 0) {
            std::cout << "Available GPUs on your machine:" << std::endl;
            for(int j = 0; j < nDevices; j++) {
                cudaDeviceProp prop;
                cudaGetDeviceProperties(&prop, j); CUERR;
                std::cout << "Id " << j << " : " << prop.name << std::endl;
            }

            for(int invalidid : invalidIds) {
                deviceIds.erase(std::find(deviceIds.begin(), deviceIds.end(), invalidid));
            }
        }

        return deviceIds;
    }

    

    template<class T>
    void printDataStructureMemoryUsage(const T& datastructure, const std::string& name){
    	auto toGB = [](std::size_t bytes){
    			    double gb = bytes / 1024. / 1024. / 1024.0;
    			    return gb;
    		    };

        auto memInfo = datastructure.getMemoryInfo();
        
        std::cout << name << " memory usage: " << toGB(memInfo.host) << " GB on host\n";
        for(const auto& pair : memInfo.device){
            std::cout << name << " memory usage: " << toGB(pair.second) << " GB on device " << pair.first << '\n';
        }
    }

    void performCorrection(
                            CorrectionOptions correctionOptions,
                            RuntimeOptions runtimeOptions,
                            MemoryOptions memoryOptions,
                            FileOptions fileOptions,
                            GoodAlignmentProperties goodAlignmentProperties){

        std::cout << "Running CARE GPU" << std::endl;

        if(runtimeOptions.deviceIds.size() == 0){
            std::cout << "No device ids found. Abort!" << std::endl;
            return;
        }

        std::uint64_t maximumNumberOfReads = fileOptions.nReads;
        int maximumSequenceLength = fileOptions.maximum_sequence_length;
        int minimumSequenceLength = fileOptions.minimum_sequence_length;
        bool scanned = false;

        if(fileOptions.load_binary_reads_from == ""){

            if(maximumNumberOfReads == 0 || maximumSequenceLength == 0 || minimumSequenceLength == 0) {
                std::cout << "STEP 0: Determine input size" << std::endl;
                
                std::cout << "Scanning file(s) to get number of reads and min/max sequence length." << std::endl;

                maximumNumberOfReads = 0;
                maximumSequenceLength = 0;
                minimumSequenceLength = std::numeric_limits<int>::max();

                for(const auto& inputfile : fileOptions.inputfiles){
                    auto prop = getSequenceFileProperties(inputfile, runtimeOptions.showProgress);
                    maximumNumberOfReads += prop.nReads;
                    maximumSequenceLength = std::max(maximumSequenceLength, prop.maxSequenceLength);
                    minimumSequenceLength = std::min(minimumSequenceLength, prop.minSequenceLength);

                    std::cout << "----------------------------------------\n";
                    std::cout << "File: " << inputfile << "\n";
                    std::cout << "Reads: " << prop.nReads << "\n";
                    std::cout << "Minimum sequence length: " << prop.minSequenceLength << "\n";
                    std::cout << "Maximum sequence length: " << prop.maxSequenceLength << "\n";
                    std::cout << "----------------------------------------\n";

                    //result.inputFileProperties.emplace_back(prop);
                }

                scanned = true;
            }else{
                //std::cout << "Using the supplied max number of reads and min/max sequence length." << std::endl;
            }
        }

        /*
            Step 1: 
            - load all reads from all input files into (gpu-)memory
            - construct minhash signatures of all reads and store them in hash tables
        */

        helpers::CpuTimer step1timer("STEP1");

        std::cout << "STEP 1: Database construction" << std::endl;

        helpers::CpuTimer buildReadStorageTimer("build_readstorage");

        gpu::DistributedReadStorage readStorage(
            runtimeOptions.deviceIds, 
            maximumNumberOfReads, 
            correctionOptions.useQualityScores, 
            minimumSequenceLength, 
            maximumSequenceLength
        );

        if(fileOptions.load_binary_reads_from != ""){

            readStorage.loadFromFile(fileOptions.load_binary_reads_from, runtimeOptions.deviceIds);

            if(correctionOptions.useQualityScores && !readStorage.canUseQualityScores())
                throw std::runtime_error("Quality scores are required but not present in preprocessed reads file!");
            if(!correctionOptions.useQualityScores && readStorage.canUseQualityScores())
                std::cerr << "Warning. The loaded preprocessed reads file contains quality scores, but program does not use them!\n";

            std::cout << "Loaded preprocessed reads from " << fileOptions.load_binary_reads_from << std::endl;

            readStorage.constructionIsComplete();
        }else{
            readStorage.construct(
                fileOptions.inputfiles,
                correctionOptions.useQualityScores,
                maximumNumberOfReads,
                minimumSequenceLength,
                maximumSequenceLength,
                runtimeOptions.threads,
                runtimeOptions.showProgress
            );
        }

        if(fileOptions.save_binary_reads_to != "") {
            std::cout << "Saving reads to file " << fileOptions.save_binary_reads_to << std::endl;
            helpers::CpuTimer timer("save_to_file");
            readStorage.saveToFile(fileOptions.save_binary_reads_to);
            timer.print();
    		std::cout << "Saved reads" << std::endl;
        }

        buildReadStorageTimer.print();
        
        SequenceFileProperties totalInputFileProperties;

        totalInputFileProperties.nReads = readStorage.getNumberOfReads();
        totalInputFileProperties.maxSequenceLength = readStorage.getStatistics().maximumSequenceLength;
        totalInputFileProperties.minSequenceLength = readStorage.getStatistics().minimumSequenceLength;

        if(!scanned){
            std::cout << "Determined the following read properties:\n";
            std::cout << "----------------------------------------\n";
            std::cout << "Total number of reads: " << totalInputFileProperties.nReads << "\n";
            std::cout << "Minimum sequence length: " << totalInputFileProperties.minSequenceLength << "\n";
            std::cout << "Maximum sequence length: " << totalInputFileProperties.maxSequenceLength << "\n";
            std::cout << "----------------------------------------\n";
        }

        if(correctionOptions.autodetectKmerlength){
            const int maxlength = totalInputFileProperties.maxSequenceLength;

            auto getKmerSizeForHashing = [](int maximumReadLength){
                if(maximumReadLength < 160){
                    return 20;
                }else{
                    return 32;
                }
            };

            correctionOptions.kmerlength = getKmerSizeForHashing(maxlength);

            std::cout << "Will use k-mer length = " << correctionOptions.kmerlength << " for hashing.\n";
        }

        std::cout << "Reads with ambiguous bases: " << readStorage.getNumberOfReadsWithN() << std::endl;
        

        printDataStructureMemoryUsage(readStorage, "reads");


        helpers::CpuTimer buildMinhasherTimer("build_minhasher");
        gpu::GpuMinhasher newGpuMinhasher(
            correctionOptions.kmerlength, 
            calculateResultsPerMapThreshold(correctionOptions.estimatedCoverage)
        );

        if(fileOptions.load_hashtables_from != ""){

            std::ifstream is(fileOptions.load_hashtables_from);
            assert((bool)is);

            const int loadedMaps = newGpuMinhasher.loadFromStream(is, correctionOptions.numHashFunctions);

            std::cout << "Loaded " << loadedMaps << " hash tables from " << fileOptions.load_hashtables_from << std::endl;
        }else{
            newGpuMinhasher.construct(
                fileOptions,
                runtimeOptions,
                memoryOptions,
                totalInputFileProperties.nReads, 
                correctionOptions,
                readStorage
            );

            if(correctionOptions.mustUseAllHashfunctions 
                && correctionOptions.numHashFunctions != newGpuMinhasher.getNumberOfMaps()){
                std::cout << "Cannot use specified number of hash functions (" 
                    << correctionOptions.numHashFunctions <<")\n";
                std::cout << "Abort!\n";
                return;
            }
        }

        if(fileOptions.save_hashtables_to != "") {
            std::cout << "Saving minhasher to file " << fileOptions.save_hashtables_to << std::endl;
            std::ofstream os(fileOptions.save_hashtables_to);
            assert((bool)os);
            helpers::CpuTimer timer("save_to_file");
            newGpuMinhasher.writeToStream(os);
            timer.print();

    		std::cout << "Saved minhasher" << std::endl;
        }

        printDataStructureMemoryUsage(newGpuMinhasher, "hash tables");



        buildMinhasherTimer.print();

        step1timer.print();



#if 0

        maximumNumberOfReads = totalInputFileProperties.nReads;
        maximumSequenceLength = totalInputFileProperties.maxSequenceLength;
        minimumSequenceLength = totalInputFileProperties.minSequenceLength;

        // timing of querying all reads. Executing on gpu 0

        const int deviceId = 0;
        cudaSetDevice(deviceId); CUERR;

        cudaStream_t stream;
        cudaStreamCreate(&stream); CUERR;


        const int maxbatchsize = 1000; // number of reads to query simultaneously

        {

            using DeviceBufferRN = SimpleAllocationDevice<read_number, 0>;
            using PinnedBufferRN = SimpleAllocationPinnedHost<read_number, 0>;
            using DeviceBufferUint = SimpleAllocationDevice<unsigned int, 0>;
            using DeviceBufferInt = SimpleAllocationDevice<int, 0>;


            const int encodedSequencePitchInInts = getEncodedNumInts2Bit(maximumSequenceLength);
            const int numHashFunctions = newGpuMinhasher.getNumberOfMaps();
            const int resultsPerMap = calculateResultsPerMapThreshold(correctionOptions.estimatedCoverage);
            const int maxResultIds = resultsPerMap * numHashFunctions * maxbatchsize;


            DeviceBufferRN d_readIds(maxbatchsize);
            PinnedBufferRN h_readIds(maxbatchsize);
            DeviceBufferUint d_encodedSequences(encodedSequencePitchInInts * maxbatchsize);
            DeviceBufferInt d_sequenceLengths(maxbatchsize);
            
            DeviceBufferRN d_queryresults(maxResultIds);
            DeviceBufferInt d_numResultsPerReadId(maxbatchsize);
            DeviceBufferInt d_numResultsPerReadIdExclPrefixSum(maxbatchsize+1); // plus total count as last element


            SimpleAllocationDevice<std::uint64_t, 0> d_normalhashvalues(numHashFunctions * maxbatchsize);
            SimpleAllocationDevice<std::uint64_t, 0> d_uniquehashvalues(numHashFunctions * maxbatchsize);
            SimpleAllocationDevice<std::uint64_t, 0> d_temp(numHashFunctions * maxbatchsize);
            SimpleAllocationDevice<int, 0> d_hashfuncids(numHashFunctions * maxbatchsize);
            SimpleAllocationDevice<int, 0> d_signaturesizes(maxbatchsize);

            SimpleAllocationPinnedHost<int, 0> h_signaturesizes(maxbatchsize);

            const int threadPoolSize = std::max(1, runtimeOptions.threads - 3*int(runtimeOptions.deviceIds.size()));

            ThreadPool threadPool(threadPoolSize);
            auto sequenceGatherHandle = readStorage.makeGatherHandleSequences();
            auto minhasherQueryHandle = gpu::GpuMinhasher::makeQueryHandle();
            minhasherQueryHandle.resize(newGpuMinhasher, maxbatchsize, threadPoolSize);

            ThreadPool::ParallelForHandle pforHandle;

            cpu::RangeGenerator<read_number> readIdGenerator(maximumNumberOfReads);

            auto showProgress = [&](std::int64_t totalCount, int seconds){
                if(runtimeOptions.showProgress){
    
                    int hours = seconds / 3600;
                    seconds = seconds % 3600;
                    int minutes = seconds / 60;
                    seconds = seconds % 60;
                    
                    printf("Processed %10lu of %10lu reads (Runtime: %03d:%02d:%02d)\r",
                    totalCount, maximumNumberOfReads,
                    hours, minutes, seconds);
                }
            };
    
            auto updateShowProgressInterval = [](auto duration){
                return duration;
            };
    
            ProgressThread<std::int64_t> progressThread(maximumNumberOfReads, showProgress, updateShowProgressInterval);
    
            cudaEvent_t eventbegin, eventend;
            cudaEventCreate(&eventbegin); CUERR;
            cudaEventCreate(&eventend); CUERR;

            float minhashtimeold = 0;
            float minhashtimenew = 0;

            std::map<int, int> numHashvaluesToFrequencyMap;
            
            while(!readIdGenerator.empty()){                

                auto hreadIdsEnd = readIdGenerator.next_n_into_buffer(
                    maxbatchsize, 
                    h_readIds.get()
                );

                const int currentbatchsize = std::distance(h_readIds.get(), hreadIdsEnd);

                cudaMemcpyAsync(
                    d_readIds.get(),
                    h_readIds.get(),
                    sizeof(read_number) * currentbatchsize,
                    H2D,
                    stream
                ); CUERR;


                readStorage.gatherSequenceDataToGpuBufferAsync(
                    &threadPool,
                    sequenceGatherHandle,
                    d_encodedSequences.get(),
                    encodedSequencePitchInInts,
                    h_readIds.get(),
                    d_readIds.get(),
                    currentbatchsize,
                    deviceId,
                    stream
                );
        
                readStorage.gatherSequenceLengthsToGpuBufferAsync(
                    d_sequenceLengths.get(),
                    deviceId,
                    d_readIds.get(),
                    currentbatchsize,         
                    stream
                );

                ParallelForLoopExecutor parallelFor(&threadPool, &pforHandle);

                cudaEventRecord(eventbegin, stream); CUERR;

                callMinhashSignaturesKernel_async(
                    d_normalhashvalues.get(),
                    numHashFunctions,
                    d_encodedSequences.get(),
                    encodedSequencePitchInInts,
                    currentbatchsize,
                    d_sequenceLengths.get(),
                    correctionOptions.kmerlength,
                    numHashFunctions,
                    stream
                );

                cudaEventRecord(eventend, stream); CUERR;

                cudaEventSynchronize(eventend); CUERR;

                float deltaold = 0;
                cudaEventElapsedTime(&deltaold, eventbegin, eventend);

                minhashtimeold += deltaold;


                cudaEventRecord(eventbegin, stream); CUERR;

                callUniqueMinhashSignaturesKernel_async(
                    d_temp.get(),
                    d_uniquehashvalues.get(),
                    numHashFunctions,
                    d_hashfuncids.get(),
                    numHashFunctions,
                    d_signaturesizes.get(),
                    d_encodedSequences.get(),
                    encodedSequencePitchInInts,
                    currentbatchsize,
                    d_sequenceLengths.get(),
                    correctionOptions.kmerlength,
                    numHashFunctions,
                    stream
                );

                cudaEventRecord(eventend, stream); CUERR;

                cudaEventSynchronize(eventend); CUERR;

                float deltanew = 0;
                cudaEventElapsedTime(&deltanew, eventbegin, eventend);

                minhashtimenew += deltanew;

                //check 

                {
                    generic_kernel<<<currentbatchsize, 64,0, stream>>>(
                        [ = ,
                            d_normalhashvalues = d_normalhashvalues.get(),
                            d_uniquehashvalues = d_uniquehashvalues.get(),
                            d_hashfuncids = d_hashfuncids.get(),
                            d_signaturesizes = d_signaturesizes.get()
                        ] __device__ (){

                            for(int s = blockIdx.x; s < currentbatchsize; s += gridDim.x){
                                const int signaturesize = d_signaturesizes[s];

                                // printf("old: ");
                                // for(int k = 0; k < numHashFunctions; k += 1){
                                //     printf("%llu ", d_normalhashvalues[numHashFunctions * s + k]);
                                // }
                                // printf("\n");


                                // printf("new(%d): ", signaturesize);
                                // for(int k = 0; k < signaturesize; k += 1){
                                //     printf("(%llu %d) ", 
                                //         d_uniquehashvalues[numHashFunctions * s + k], 
                                //         d_hashfuncids[numHashFunctions * s + k]
                                //     );
                                // }
                                // printf("\n");

                                for(int k = threadIdx.x; k < signaturesize; k += blockDim.x){
                                    const auto newhashvalue = d_uniquehashvalues[numHashFunctions * s + k];
                                    const int hashfuncid = d_hashfuncids[numHashFunctions * s + k];

                                    const auto oldhashvalue = d_normalhashvalues[numHashFunctions * s + hashfuncid];

                                    if(newhashvalue != oldhashvalue){
                                        printf("error at s=%d, k=%d, hashfuncid=%d, old=%llu, new=%llu\n", 
                                            s,k, hashfuncid, oldhashvalue,newhashvalue);
                                    }
                                    assert(newhashvalue == oldhashvalue);
                                }
                            }
                        }
                    );
                    CUERR;
                }

                cudaMemcpyAsync(
                    h_signaturesizes.get(),
                    d_signaturesizes.get(),
                    d_signaturesizes.sizeInBytes(),
                    D2H,
                    stream
                );

                cudaStreamSynchronize(stream);

                progressThread.addProgress(currentbatchsize);

                for(int i = 0; i < currentbatchsize; i++){
                    numHashvaluesToFrequencyMap[h_signaturesizes[i]]++;
                }


            }

            progressThread.finished(); 
        
            std::cout << std::endl;

            std::cout << "Minhashing old took " << minhashtimeold << " ms\n";
            std::cout << "Minhashing new took " << minhashtimenew << " ms\n";

            for(auto pair : numHashvaluesToFrequencyMap){
                std::cout << pair.first << " " << pair.second << "\n";
            }

            cudaEventDestroy(eventbegin); CUERR;
            cudaEventDestroy(eventend); CUERR;

            gpu::GpuMinhasher::destroyQueryHandle(minhasherQueryHandle);

        }




        cudaStreamDestroy(stream); CUERR;





#endif 






        std::cout << "STEP 2: Error correction" << std::endl;

        helpers::CpuTimer step2timer("STEP2");

        auto partialResults = gpu::correct_gpu(
            goodAlignmentProperties, 
            correctionOptions,
            runtimeOptions, 
            fileOptions, 
            memoryOptions,
            totalInputFileProperties,
            //minhasher, 
            newGpuMinhasher,
            readStorage
        );

        step2timer.print();

        std::cout << "Correction throughput : ~" << (totalInputFileProperties.nReads / step2timer.elapsed()) << " reads/second.\n";

        newGpuMinhasher.destroy();
        readStorage.destroy();

        //Merge corrected reads with input file to generate output file

        const std::size_t availableMemoryInBytes = getAvailableMemoryInKB() * 1024;
        std::size_t memoryForSorting = 0;

        if(availableMemoryInBytes > 1*(std::size_t(1) << 30)){
            memoryForSorting = availableMemoryInBytes - 1*(std::size_t(1) << 30);
        }               

        std::cout << "STEP 3: Constructing output file(s)" << std::endl;

        helpers::CpuTimer step3timer("STEP3");

        std::vector<FileFormat> formats;
        for(const auto& inputfile : fileOptions.inputfiles){
            formats.emplace_back(getFileFormat(inputfile));
        }
        std::vector<std::string> outputfiles;
        for(const auto& outputfilename : fileOptions.outputfilenames){
            outputfiles.emplace_back(fileOptions.outputdirectory + "/" + outputfilename);
        }
        constructOutputFileFromCorrectionResults(
            fileOptions.tempdirectory,
            fileOptions.inputfiles, 
            partialResults, 
            memoryForSorting,
            formats[0],
            outputfiles, 
            false
        );

        step3timer.print();

        std::cout << "Construction of output file(s) finished." << std::endl;

    }
















    void performExtension(
                            CorrectionOptions correctionOptions,
                            ExtensionOptions extensionOptions,
                            RuntimeOptions runtimeOptions,
                            MemoryOptions memoryOptions,
                            FileOptions fileOptions,
                            GoodAlignmentProperties goodAlignmentProperties){

        std::cout << "Running CARE EXTEND GPU" << std::endl;


        // {
        //     MemoryFileFixedSize<ExtendedRead> memfile{0, fileOptions.tempdirectory+"/tmpfile"};

        //     ExtendedRead er1;
        //     er1.readId1 = 0;
        //     er1.readId2 = 1;

        //     ExtendedRead er2;
        //     er2.readId1 = 2;
        //     er2.readId2 = 3;

        //     ExtendedRead er3;
        //     er3.readId1 = 4;
        //     er3.readId2 = 5;

        //     ExtendedRead er4;
        //     er4.readId1 = 6;
        //     er4.readId2 = 7;

        //     ExtendedRead er5;
        //     er5.readId1 = 8;
        //     er5.readId2 = 9;

        //     memfile.storeElement(std::move(er1));
        //     memfile.storeElement(std::move(er2));
        //     memfile.storeElement(std::move(er3));
        //     memfile.storeElement(std::move(er4));
        //     memfile.storeElement(std::move(er5));

        //     memfile.flush();
            
        //     if(true){
        //         auto ptrcomparator = [](const std::uint8_t* ptr1, const std::uint8_t* ptr2){
        //             read_number lid1, lid2;
        //             read_number rid1, rid2;
        //             std::memcpy(&lid1, ptr1, sizeof(read_number));
        //             std::memcpy(&lid2, ptr1, sizeof(read_number));
        //             std::memcpy(&rid1, ptr2, sizeof(read_number));
        //             std::memcpy(&rid2, ptr2, sizeof(read_number));

        //             std::cerr << "ptrcomparator: (" << lid1 << "," << lid2 << ") - (" << rid1 << "," << rid2 << ")\n";
                    
        //             if(lid1 < rid1) return true;
        //             if(lid1 > rid1) return false;
        //             if(lid2 < rid2) return true;
        //             return false;
        //         };

        //         auto elementcomparator = [](const auto& l, const auto& r){
        //             std::cerr << "elementcomp: (" << l.readId1 << "," << l.readId2 << ") - (" << r.readId1 << "," << r.readId2 << ")\n";
        //             if(l.readId1 < r.readId1) return true;
        //             if(l.readId1 > r.readId1) return false;
        //             if(l.readId2 < r.readId2) return true;
        //             return false;
        //         };

        //         TIMERSTARTCPU(sort_results_by_read_id);
        //         memfile.sort(fileOptions.tempdirectory, 10000000, ptrcomparator, elementcomparator);
        //         TIMERSTOPCPU(sort_results_by_read_id);
        //     }

            

        //     auto partialResultsReader = memfile.makeReader();

        //     std::cerr << "in mem: " << memfile.getNumElementsInMemory() << ", in file: " << memfile.getNumElementsInFile() << "\n";

        //     while(partialResultsReader.hasNext()){
        //         ExtendedRead extendedRead = *(partialResultsReader.next());
        //         std::cerr << extendedRead.readId1 << " " << extendedRead.readId2 << "\n";
        //     }
        // }

        std::uint64_t maximumNumberOfReads = fileOptions.nReads;
        int maximumSequenceLength = fileOptions.maximum_sequence_length;
        int minimumSequenceLength = fileOptions.minimum_sequence_length;
        bool scanned = false;

        if(fileOptions.load_binary_reads_from == ""){

            if(maximumNumberOfReads == 0 || maximumSequenceLength == 0 || minimumSequenceLength == 0) {
                std::cout << "STEP 0: Determine input size" << std::endl;
                
                std::cout << "Scanning file(s) to get number of reads and min/max sequence length." << std::endl;

                maximumNumberOfReads = 0;
                maximumSequenceLength = 0;
                minimumSequenceLength = std::numeric_limits<int>::max();

                for(const auto& inputfile : fileOptions.inputfiles){
                    auto prop = getSequenceFileProperties(inputfile, runtimeOptions.showProgress);
                    maximumNumberOfReads += prop.nReads;
                    maximumSequenceLength = std::max(maximumSequenceLength, prop.maxSequenceLength);
                    minimumSequenceLength = std::min(minimumSequenceLength, prop.minSequenceLength);

                    std::cout << "----------------------------------------\n";
                    std::cout << "File: " << inputfile << "\n";
                    std::cout << "Reads: " << prop.nReads << "\n";
                    std::cout << "Minimum sequence length: " << prop.minSequenceLength << "\n";
                    std::cout << "Maximum sequence length: " << prop.maxSequenceLength << "\n";
                    std::cout << "----------------------------------------\n";

                    //result.inputFileProperties.emplace_back(prop);
                }

                scanned = true;
            }else{
                //std::cout << "Using the supplied max number of reads and min/max sequence length." << std::endl;
            }
        }

        std::cout << "STEP 1: Database construction" << std::endl;

        helpers::CpuTimer step1Timer("STEP1");

        helpers::CpuTimer buildReadStorageTimer("build_readstorage");


        // care::cpu::ContiguousReadStorage readStorage(
        //     maximumNumberOfReads, 
        //     correctionOptions.useQualityScores, 
        //     minimumSequenceLength, 
        //     maximumSequenceLength
        // );

        gpu::DistributedReadStorage readStorage(
            runtimeOptions.deviceIds, 
            maximumNumberOfReads, 
            correctionOptions.useQualityScores, 
            minimumSequenceLength, 
            maximumSequenceLength
        );

        if(fileOptions.load_binary_reads_from != ""){

            readStorage.loadFromFile(fileOptions.load_binary_reads_from);

            if(correctionOptions.useQualityScores && !readStorage.canUseQualityScores())
                throw std::runtime_error("Quality scores are required but not present in preprocessed reads file!");
            if(!correctionOptions.useQualityScores && readStorage.canUseQualityScores())
                std::cerr << "Warning. The loaded preprocessed reads file contains quality scores, but program does not use them!\n";

            std::cout << "Loaded preprocessed reads from " << fileOptions.load_binary_reads_from << std::endl;

            readStorage.constructionIsComplete();
        }else{
            if(fileOptions.pairType == SequencePairType::PairedEnd && fileOptions.inputfiles.size() == 2){
                readStorage.constructPaired(
                    fileOptions.inputfiles,
                    correctionOptions.useQualityScores,
                    maximumNumberOfReads,
                    minimumSequenceLength,
                    maximumSequenceLength,
                    runtimeOptions.threads,
                    runtimeOptions.showProgress
                );
            }else{
                readStorage.construct(
                    fileOptions.inputfiles,
                    correctionOptions.useQualityScores,
                    maximumNumberOfReads,
                    minimumSequenceLength,
                    maximumSequenceLength,
                    runtimeOptions.threads,
                    runtimeOptions.showProgress
                );
            }
        }

        buildReadStorageTimer.print();

        if(fileOptions.save_binary_reads_to != "") {
            std::cout << "Saving reads to file " << fileOptions.save_binary_reads_to << std::endl;
            helpers::CpuTimer timer("save_to_file");
            readStorage.saveToFile(fileOptions.save_binary_reads_to);
            timer.print();
    		std::cout << "Saved reads" << std::endl;
        }

        
        
        SequenceFileProperties totalInputFileProperties;

        totalInputFileProperties.nReads = readStorage.getNumberOfReads();
        totalInputFileProperties.maxSequenceLength = readStorage.getStatistics().maximumSequenceLength;
        totalInputFileProperties.minSequenceLength = readStorage.getStatistics().minimumSequenceLength;

        if(!scanned){
            std::cout << "Determined the following read properties:\n";
            std::cout << "----------------------------------------\n";
            std::cout << "Total number of reads: " << totalInputFileProperties.nReads << "\n";
            std::cout << "Minimum sequence length: " << totalInputFileProperties.minSequenceLength << "\n";
            std::cout << "Maximum sequence length: " << totalInputFileProperties.maxSequenceLength << "\n";
            std::cout << "----------------------------------------\n";
        }

        if(correctionOptions.autodetectKmerlength){
            const int maxlength = totalInputFileProperties.maxSequenceLength;

            auto getKmerSizeForHashing = [](int maximumReadLength){
                if(maximumReadLength < 160){
                    return 20;
                }else{
                    return 32;
                }
            };

            correctionOptions.kmerlength = getKmerSizeForHashing(maxlength);

            std::cout << "Will use k-mer length = " << correctionOptions.kmerlength << " for hashing.\n";
        }

        std::cout << "Reads with ambiguous bases: " << readStorage.getNumberOfReadsWithN() << std::endl;        

        printDataStructureMemoryUsage(readStorage, "reads");


        helpers::CpuTimer buildMinhasherTimer("build_minhasher");
        gpu::GpuMinhasher minhasher(
            correctionOptions.kmerlength, 
            calculateResultsPerMapThreshold(correctionOptions.estimatedCoverage)
        );

        if(fileOptions.load_hashtables_from != ""){

            std::ifstream is(fileOptions.load_hashtables_from);
            assert((bool)is);

            const int loadedMaps = minhasher.loadFromStream(is, correctionOptions.numHashFunctions);

            std::cout << "Loaded " << loadedMaps << " hash tables from " << fileOptions.load_hashtables_from << std::endl;
        }else{
            minhasher.construct(
                fileOptions,
                runtimeOptions,
                memoryOptions,
                totalInputFileProperties.nReads, 
                correctionOptions,
                readStorage
            );

            if(correctionOptions.mustUseAllHashfunctions 
                && correctionOptions.numHashFunctions != minhasher.getNumberOfMaps()){
                std::cout << "Cannot use specified number of hash functions (" 
                    << correctionOptions.numHashFunctions <<")\n";
                std::cout << "Abort!\n";
                return;
            }
        }

        buildMinhasherTimer.print();

        if(fileOptions.save_hashtables_to != "") {
            std::cout << "Saving minhasher to file " << fileOptions.save_hashtables_to << std::endl;
            std::ofstream os(fileOptions.save_hashtables_to);
            assert((bool)os);
            helpers::CpuTimer timer("save_to_file");
            minhasher.writeToStream(os);
            timer.print();
    		std::cout << "Saved minhasher" << std::endl;
        }



        printDataStructureMemoryUsage(minhasher, "hash tables");

        step1Timer.print();

        std::cout << "STEP 2: Read extension" << std::endl;

        helpers::CpuTimer step2Timer("STEP2");

        auto partialResults = gpu::extend_gpu(
            goodAlignmentProperties, 
            correctionOptions,
            extensionOptions,
            runtimeOptions, 
            fileOptions, 
            memoryOptions, 
            totalInputFileProperties,
            minhasher, 
            readStorage
        );

        step2Timer.print();

        minhasher.destroy();
        readStorage.destroy();

        const std::size_t availableMemoryInBytes2 = getAvailableMemoryInKB() * 1024;
        std::size_t memoryForSorting = 0;

        if(availableMemoryInBytes2 > 1*(std::size_t(1) << 30)){
            memoryForSorting = availableMemoryInBytes2 - 1*(std::size_t(1) << 30);
        }

        (void)memoryForSorting;

        std::cout << "STEP 3: Constructing output file(s)" << std::endl;

        helpers::CpuTimer step3Timer("STEP3");

        std::vector<FileFormat> formats;
        for(const auto& inputfile : fileOptions.inputfiles){
            formats.emplace_back(getFileFormat(inputfile));
        }
        std::vector<std::string> outputfiles;
        for(const auto& outputfilename : fileOptions.outputfilenames){
            outputfiles.emplace_back(fileOptions.outputdirectory + "/" + outputfilename);
        }

        auto outputFormat = getFileFormat(fileOptions.inputfiles[0]);
        //no gz output
        if(outputFormat == FileFormat::FASTQGZ)
            outputFormat = FileFormat::FASTQ;
        if(outputFormat == FileFormat::FASTAGZ)
            outputFormat = FileFormat::FASTA;


        const std::string extendedOutputfile = fileOptions.outputdirectory + "/" + fileOptions.extendedReadsOutputfilename;

        constructOutputFileFromExtensionResults(
            fileOptions.tempdirectory,
            fileOptions.inputfiles,            
            partialResults, 
            memoryForSorting,
            outputFormat, 
            extendedOutputfile,
            outputfiles,
            fileOptions.pairType, 
            false
        );


        step3Timer.print();

        std::cout << "Construction of output file(s) finished." << std::endl;

    }

}
