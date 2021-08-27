#include <gpu/dispatch_care_extend_gpu.cuh>
#include <gpu/gpuminhasherconstruction.cuh>
#include <gpu/fakegpuminhasher.cuh>
#include <gpu/cudaerrorcheck.cuh>

#include <config.hpp>
#include <options.hpp>
#include <readlibraryio.hpp>
#include <memorymanagement.hpp>

#include <gpu/multigpureadstorage.cuh>
#include <chunkedreadstorageconstruction.hpp>
#include <chunkedreadstorage.hpp>

#include <rangegenerator.hpp>

#include <gpu/readextension_gpu.hpp>
#include <extensionresultprocessing.hpp>
#include <sortserializedresults.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

#include <experimental/filesystem>

namespace filesys = std::experimental::filesystem;

namespace care{

    namespace extension{

        std::vector<int> getUsableDeviceIds(std::vector<int> deviceIds){
            int nDevices;

            CUDACHECK(cudaGetDeviceCount(&nDevices));

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
                    CUDACHECK(cudaGetDeviceProperties(&prop, j));
                    std::cout << "Id " << j << " : " << prop.name << std::endl;
                }

                for(int invalidid : invalidIds) {
                    deviceIds.erase(std::find(deviceIds.begin(), deviceIds.end(), invalidid));
                }
            }

            //check gpu restrictions of remaining gpus
            invalidIds.clear();

            for(int id : deviceIds){
                cudaDeviceProp prop;
                cudaGetDeviceProperties(&prop, id);

                if(prop.major < 6){
                    invalidIds.emplace_back(id);
                    std::cerr << "Warning. Removing gpu id " << id << " because its not arch 6 or greater.\n";
                }

                if(prop.managedMemory != 1){
                    invalidIds.emplace_back(id);
                    std::cerr << "Warning. Removing gpu id " << id << " because it does not support managed memory. (may be required for hash table construction).\n";
                }
            }

            if(invalidIds.size() > 0) {
                for(int invalidid : invalidIds) {
                    deviceIds.erase(std::find(deviceIds.begin(), deviceIds.end(), invalidid));
                }
            }

            return deviceIds;
        }

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

    void performExtension(
        CorrectionOptions correctionOptions,
        ExtensionOptions extensionOptions,
        RuntimeOptions runtimeOptions,
        MemoryOptions memoryOptions,
        FileOptions fileOptions,
        GoodAlignmentProperties goodAlignmentProperties
    ){

        std::cout << "Running CARE EXTEND GPU" << std::endl;

        if(runtimeOptions.deviceIds.size() == 0){
            std::cout << "No device ids found. Abort!" << std::endl;
            return;
        }

        CUDACHECK(cudaSetDevice(runtimeOptions.deviceIds[0]));

        helpers::PeerAccessDebug peerAccess(runtimeOptions.deviceIds, true);
        peerAccess.enableAllPeerAccesses();

        helpers::CpuTimer step1timer("STEP1");

        std::cout << "STEP 1: Database construction" << std::endl;

        helpers::CpuTimer buildReadStorageTimer("build_readstorage");

        std::unique_ptr<ChunkedReadStorage> cpuReadStorage = constructChunkedReadStorageFromFiles(
            runtimeOptions,
            memoryOptions,
            fileOptions,
            correctionOptions.useQualityScores
        );

        buildReadStorageTimer.print();

        std::cout << "Determined the following read properties:\n";
        std::cout << "----------------------------------------\n";
        std::cout << "Total number of reads: " << cpuReadStorage->getNumberOfReads() << "\n";
        std::cout << "Minimum sequence length: " << cpuReadStorage->getSequenceLengthLowerBound() << "\n";
        std::cout << "Maximum sequence length: " << cpuReadStorage->getSequenceLengthUpperBound() << "\n";
        std::cout << "----------------------------------------\n";

        if(fileOptions.save_binary_reads_to != ""){
            std::cout << "Saving reads to file " << fileOptions.save_binary_reads_to << std::endl;
            helpers::CpuTimer timer("save_to_file");
            cpuReadStorage->saveToFile(fileOptions.save_binary_reads_to);
            timer.print();
            std::cout << "Saved reads" << std::endl;
        }

        if(correctionOptions.autodetectKmerlength){
            const int maxlength = cpuReadStorage->getSequenceLengthUpperBound();

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

        std::cout << "Reads with ambiguous bases: " << cpuReadStorage->getNumberOfReadsWithN() << std::endl;      


        std::vector<std::size_t> gpumemorylimits(runtimeOptions.deviceIds.size(), 0);

        // gpumemorylimits.resize(2);
        // std::fill(gpumemorylimits.begin(), gpumemorylimits.end(), 512000000);

        // std::vector<int> tempids2(gpumemorylimits.size(), 0);


        gpu::MultiGpuReadStorage gpuReadStorage(
            *cpuReadStorage, 
            runtimeOptions.deviceIds,
            //tempids2,
            gpumemorylimits,
            0
        );

        helpers::CpuTimer buildMinhasherTimer("build_minhasher");

        auto minhasherAndType = gpu::constructGpuMinhasherFromGpuReadStorage(
            fileOptions,
            runtimeOptions,
            memoryOptions,
            correctionOptions,
            gpuReadStorage,
            gpu::GpuMinhasherType::Multi
        );

        gpu::GpuMinhasher* gpuMinhasher = minhasherAndType.first.get();

        buildMinhasherTimer.print();

        std::cout << "GpuMinhasher can use " << gpuMinhasher->getNumberOfMaps() << " maps\n";

        if(gpuMinhasher->getNumberOfMaps() <= 0){
            std::cout << "Cannot construct a single gpu hashtable. Abort!" << std::endl;
            return;
        }

        if(correctionOptions.mustUseAllHashfunctions 
            && correctionOptions.numHashFunctions != gpuMinhasher->getNumberOfMaps()){
            std::cout << "Cannot use specified number of hash functions (" 
                << correctionOptions.numHashFunctions <<")\n";
            std::cout << "Abort!\n";
            return;
        }

        if(minhasherAndType.second == gpu::GpuMinhasherType::Fake){

            gpu::FakeGpuMinhasher* fakeGpuMinhasher = dynamic_cast<gpu::FakeGpuMinhasher*>(gpuMinhasher);
            assert(fakeGpuMinhasher != nullptr);

            if(fileOptions.save_hashtables_to != "") {
                std::cout << "Saving minhasher to file " << fileOptions.save_hashtables_to << std::endl;
                std::ofstream os(fileOptions.save_hashtables_to);
                assert((bool)os);
                helpers::CpuTimer timer("save_to_file");
                fakeGpuMinhasher->writeToStream(os);
                timer.print();

                std::cout << "Saved minhasher" << std::endl;
            }

        }

        printDataStructureMemoryUsage(*gpuMinhasher, "hash tables");        

        step1timer.print();

        //After minhasher is constructed, remaining gpu memory can be used to store reads

        auto getGpuMemoryLimits = [&](){
            const std::size_t numGpus = runtimeOptions.deviceIds.size();
            std::vector<std::size_t> limits(numGpus, 0);

            for(std::size_t i = 0; i < numGpus; i++){
                std::size_t total = 0;
                cudaMemGetInfo(&limits[i], &total);

                std::size_t safety = 1 << 30; //leave 1 GB for algorithm
                if(limits[i] > safety){
                    limits[i] -= safety;
                }else{
                    limits[i] = 0;
                }
            }
            return limits;
        };

        gpumemorylimits = getGpuMemoryLimits();

        std::size_t memoryLimitHost = memoryOptions.memoryTotalLimit 
            - cpuReadStorage->getMemoryInfo().host
            - gpuMinhasher->getMemoryInfo().host;

        // gpumemorylimits.resize(2);
        // std::fill(gpumemorylimits.begin(), gpumemorylimits.end(), 512000000);

        // std::vector<int> tempids(gpumemorylimits.size(), 0);

        helpers::CpuTimer cpugputimer("cpu->gpu reads");
        cpugputimer.start();
        gpuReadStorage.rebuild(
            *cpuReadStorage,
            runtimeOptions.deviceIds, 
            //tempids,
            gpumemorylimits,
            memoryLimitHost
        );

        gpumemorylimits = getGpuMemoryLimits();
        
        if(runtimeOptions.replicateGpuData){
            if(gpuReadStorage.trySequenceReplication(gpumemorylimits)){
                std::cerr << "Replicated gpu read sequences to each gpu\n";
                gpumemorylimits = getGpuMemoryLimits();
            }
            
            if(gpuReadStorage.tryQualityReplication(gpumemorylimits)){
                std::cerr << "Replicated gpu read qualities to each gpu\n";
                gpumemorylimits = getGpuMemoryLimits();
            }
        }
        cpugputimer.print();

        std::cout << "constructed gpu readstorage " << std::endl;

        printDataStructureMemoryUsage(gpuReadStorage, "reads");

        if(gpuReadStorage.isStandalone()){
            cpuReadStorage.reset();
        }

        std::cout << "STEP 2: Read extension" << std::endl;

        helpers::CpuTimer step2timer("STEP2");

        auto partialResults = gpu::extend_gpu(
            goodAlignmentProperties, 
            correctionOptions,
            extensionOptions,
            runtimeOptions, 
            fileOptions, 
            memoryOptions, 
            *gpuMinhasher, 
            gpuReadStorage
        );

        step2timer.print();

        std::cout << "Extension throughput : ~" << (gpuReadStorage.getNumberOfReads() / step2timer.elapsed()) << " reads/second.\n"; //TODO: paired end? numreads / 2 ?
    
        std::cerr << "Constructed " << partialResults.size() << " extensions. ";
        std::cerr << "They occupy a total of " << (partialResults.dataBytes() + partialResults.offsetBytes()) << " bytes\n";

        minhasherAndType.first.reset();
        gpuMinhasher = nullptr;
        gpuReadStorage.destroy();
        cpuReadStorage.reset();   

        //Merge corrected reads with input file to generate output file

        const std::size_t availableMemoryInBytes = getAvailableMemoryInKB() * 1024;
        const auto partialResultMemUsage = partialResults.getMemoryInfo();

        // std::cerr << "availableMemoryInBytes = " << availableMemoryInBytes << "\n";
        // std::cerr << "memoryLimitOption = " << memoryOptions.memoryTotalLimit << "\n";
        // std::cerr << "partialResultMemUsage = " << partialResultMemUsage.host << "\n";

        std::size_t memoryForSorting = std::min(
            availableMemoryInBytes,
            memoryOptions.memoryTotalLimit - partialResultMemUsage.host
        );

        if(memoryForSorting > 1*(std::size_t(1) << 30)){
            memoryForSorting = memoryForSorting - 1*(std::size_t(1) << 30);
        }
        std::cerr << "memoryForSorting = " << memoryForSorting << "\n"; 

        std::cout << "STEP 3: Constructing output file(s)" << std::endl;

        helpers::CpuTimer step3Timer("STEP3");

        helpers::CpuTimer sorttimer("sort_results_by_read_id");

        sortSerializedResultsByReadIdAscending<ExtendedRead>(
            partialResults,
            memoryForSorting
        );

        sorttimer.print();

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

        //read extender does not produce longer quality scores -> only output fasta format
        //if(outputFormat == FileFormat::FASTQGZ)
           // outputFormat = FileFormat::FASTQ;

        const std::string extendedOutputfile = fileOptions.outputdirectory + "/" + fileOptions.extendedReadsOutputfilename;

        constructOutputFileFromExtensionResults(
            fileOptions.inputfiles,            
            partialResults, 
            outputFormat, 
            extendedOutputfile,
            outputfiles,
            fileOptions.pairType, 
            fileOptions.mergedoutput
        );

        step3Timer.print();

        std::cout << "Construction of output file(s) finished." << std::endl;

    }

}
