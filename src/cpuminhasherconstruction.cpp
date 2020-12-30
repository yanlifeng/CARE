
#include <cpuminhasherconstruction.hpp>

#include <cpuminhasher.hpp>
#include <ordinaryminhasher.hpp>

#include <minhasherlimit.hpp>

#include <options.hpp>

#include <memory>
#include <utility>


namespace care{   

        std::unique_ptr<OrdinaryCpuMinhasher>
        constructOrdinaryCpuMinhasherFromCpuReadStorage(
            const CorrectionOptions& correctionOptions,
            const FileOptions& fileOptions,
            const SequenceFileProperties& totalInputFileProperties,
            const RuntimeOptions& runtimeOptions,
            const MemoryOptions& memoryOptions,
            const cpu::ContiguousReadStorage& cpuReadStorage
        ){
            auto cpuMinhasher = std::make_unique<OrdinaryCpuMinhasher>(
                totalInputFileProperties.nReads,
                calculateResultsPerMapThreshold(correctionOptions.estimatedCoverage),
                correctionOptions.kmerlength
            );

            if(fileOptions.load_hashtables_from != ""){

                std::ifstream is(fileOptions.load_hashtables_from);
                assert((bool)is);
    
                const int loadedMaps = cpuMinhasher->loadFromStream(is, correctionOptions.numHashFunctions);
    
                std::cout << "Loaded " << loadedMaps << " hash tables from " << fileOptions.load_hashtables_from << std::endl;
            }else{
                cpuMinhasher->constructFromReadStorage(
                    fileOptions,
                    runtimeOptions,
                    memoryOptions,
                    totalInputFileProperties.nReads, 
                    correctionOptions,
                    cpuReadStorage
                );
            }

            return cpuMinhasher;
        }


        std::pair<std::unique_ptr<CpuMinhasher>, CpuMinhasherType>
        constructCpuMinhasherFromCpuReadStorage(
            const FileOptions& fileOptions,
            const RuntimeOptions& runtimeOptions,
            const MemoryOptions& memoryOptions,
            const CorrectionOptions& correctionOptions,
            const SequenceFileProperties& totalInputFileProperties,
            const cpu::ContiguousReadStorage& gpuReadStorage,
            CpuMinhasherType requestedType
        ){
            if(requestedType == CpuMinhasherType::Ordinary){
                return std::make_pair(
                    constructOrdinaryCpuMinhasherFromCpuReadStorage(
                        correctionOptions,
                        fileOptions,
                        totalInputFileProperties,
                        runtimeOptions,
                        memoryOptions,
                        gpuReadStorage
                    ),
                    CpuMinhasherType::Ordinary
                );
            }else{
                return std::make_pair(
                    constructOrdinaryCpuMinhasherFromCpuReadStorage(
                        correctionOptions,
                        fileOptions,
                        totalInputFileProperties,
                        runtimeOptions,
                        memoryOptions,
                        gpuReadStorage
                    ),
                    CpuMinhasherType::Ordinary
                );
            }
        }
    
    
}
