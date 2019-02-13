#ifndef CARE_CORRECT_HPP
#define CARE_CORRECT_HPP

#include "options.hpp"

#include "graph.hpp"

#include "sequence.hpp"
#include "sequencefileio.hpp"
#include "qualityscoreweights.hpp"
#include "tasktiming.hpp"

#include "featureextractor.hpp"
#include "bestalignment.hpp"


#include "rangegenerator.hpp"


#include <array>
#include <cstdint>
#include <sstream>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include <future>


#include "cpu_correction_thread.hpp"

#include "candidatedistribution.hpp"

namespace care{
namespace cpu{

    template<class minhasher_t,
    		 class readStorage_t,
    		 bool indels>
    void correct_cpu(const MinhashOptions& minhashOptions,
    				  const AlignmentOptions& alignmentOptions,
    				  const GoodAlignmentProperties& goodAlignmentProperties,
    				  const CorrectionOptions& correctionOptions,
    				  const RuntimeOptions& runtimeOptions,
    				  const FileOptions& fileOptions,
                      const SequenceFileProperties& sequenceFileProperties,
                      minhasher_t& minhasher,
                      readStorage_t& readStorage,
    				  std::vector<char>& readIsCorrectedVector,
    				  std::unique_ptr<std::mutex[]>& locksForProcessedFlags,
    				  std::size_t nLocksForProcessedFlags){

    	assert(indels == false);

        std::cout << "correct_cpu" << std::endl;

    	using Minhasher_t = minhasher_t;
    	using ReadStorage_t = readStorage_t;
    	//using Sequence_t = typename ReadStorage_t::Sequence_t;
    	using ReadId_t = typename ReadStorage_t::ReadId_t;
        using CPUErrorCorrectionThread_t = cpu::CPUCorrectionThread<Minhasher_t, ReadStorage_t, indels>;

    #if 1
        const int nCorrectorThreads = runtimeOptions.nCorrectorThreads;
    #else
    	const int nCorrectorThreads = 1;
    #endif

    	std::cout << "Using " << nCorrectorThreads << " corrector threads" << std::endl;

          // initialize qscore-to-weight lookup table
      	//cpu::init_weights();

        //SequenceFileProperties sequenceFileProperties = getSequenceFileProperties(fileOptions.inputfile, fileOptions.format);

        /*
            Make candidate statistics
        */

        std::uint64_t max_candidates = runtimeOptions.max_candidates;
        //std::uint64_t max_candidates = std::numeric_limits<std::uint64_t>::max();

        if(max_candidates == 0){
            std::cout << "estimating candidate cutoff" << std::endl;

            Dist<std::int64_t, std::int64_t> candidateDistribution;
            cpu::Dist2<std::int64_t, std::int64_t> candidateDistribution2;

            {
                TIMERSTARTCPU(candidateestimation);
                std::map<std::int64_t, std::int64_t> candidateHistogram
                        = getCandidateCountHistogram(minhasher,
                                                    readStorage,
                                                    sequenceFileProperties.nReads / 10,
                                                    correctionOptions.hits_per_candidate,
                                                    runtimeOptions.threads);

                TIMERSTOPCPU(candidateestimation);

                candidateDistribution = estimateDist(candidateHistogram);
                //candidateDistribution2 = cpu::estimateDist2(candidateHistogram);

                std::vector<std::pair<std::int64_t, std::int64_t>> vec(candidateHistogram.begin(), candidateHistogram.end());
                std::sort(vec.begin(), vec.end(), [](auto p1, auto p2){ return p1.second < p2.second;});

                std::ofstream of("ncandidates.txt");
                for(const auto& p : vec)
                    of << p.first << " " << p.second << '\n';
                of.flush();
            }

            std::cout << "candidates.max " << candidateDistribution.max << std::endl;
            std::cout << "candidates.average " << candidateDistribution.average << std::endl;
            std::cout << "candidates.stddev " << candidateDistribution.stddev << std::endl;

            const std::uint64_t estimatedMeanAlignedCandidates = candidateDistribution.max;
            const std::uint64_t estimatedDeviationAlignedCandidates = candidateDistribution.stddev;
            const std::uint64_t estimatedAlignmentCountThreshold = estimatedMeanAlignedCandidates
                                                            + 2.5 * estimatedDeviationAlignedCandidates;

            max_candidates = estimatedAlignmentCountThreshold;
            //max_candidates = candidateDistribution2.percentRanges[90].first;
            //std::exit(0);
        }

        std::cout << "Using candidate cutoff: " << max_candidates << std::endl;

        /*
            Spawn correction threads
        */

        std::vector<std::string> tmpfiles;
        for(int i = 0; i < nCorrectorThreads; i++){
            tmpfiles.emplace_back(fileOptions.outputfile + "_tmp_" + std::to_string(1000 + i));
        }

        cpu::RangeGenerator<ReadId_t> readIdGenerator(sequenceFileProperties.nReads);

        std::vector<CPUErrorCorrectionThread_t> cpucorrectorThreads(nCorrectorThreads);
        std::vector<char> readIsProcessedVector(readIsCorrectedVector);
        std::mutex writelock;

    	for(int threadId = 0; threadId < nCorrectorThreads; threadId++){

            //cpubatchgenerators[threadId] = BatchGenerator<ReadId_t>(ncpuReads, 1, threadId, nCpuThreads);
            typename CPUErrorCorrectionThread_t::CorrectionThreadOptions threadOpts;
            threadOpts.threadId = threadId;

            threadOpts.outputfile = tmpfiles[threadId];
            threadOpts.readIdGenerator = &readIdGenerator;
            threadOpts.minhasher = &minhasher;
            threadOpts.readStorage = &readStorage;
            threadOpts.coutLock = &writelock;
            threadOpts.readIsProcessedVector = &readIsProcessedVector;
            threadOpts.readIsCorrectedVector = &readIsCorrectedVector;
            threadOpts.locksForProcessedFlags = locksForProcessedFlags.get();
            threadOpts.nLocksForProcessedFlags = nLocksForProcessedFlags;

            cpucorrectorThreads[threadId].alignmentOptions = alignmentOptions;
            cpucorrectorThreads[threadId].goodAlignmentProperties = goodAlignmentProperties;
            cpucorrectorThreads[threadId].correctionOptions = correctionOptions;
            cpucorrectorThreads[threadId].fileOptions = fileOptions;
            cpucorrectorThreads[threadId].threadOpts = threadOpts;
            cpucorrectorThreads[threadId].fileProperties = sequenceFileProperties;
            cpucorrectorThreads[threadId].max_candidates = max_candidates;

            cpucorrectorThreads[threadId].run();
        }

        std::cout << "Correcting..." << std::endl;

        bool showProgress = runtimeOptions.showProgress;

        std::thread progressThread = std::thread([&]() -> void{
            if(!showProgress)
                return;

            std::chrono::time_point<std::chrono::system_clock> timepoint_begin = std::chrono::system_clock::now();
            std::chrono::duration<double> runtime = std::chrono::seconds(0);
            std::chrono::duration<int> sleepinterval = std::chrono::seconds(1);

            while(showProgress){
                ReadId_t progress = readIdGenerator.getCurrentUnsafe() - readIdGenerator.getBegin();

                printf("Progress: %3.2f %% %10u %10lu (Runtime: %03d:%02d:%02d)\r",
                        ((progress * 1.0 / sequenceFileProperties.nReads) * 100.0),
                        progress, sequenceFileProperties.nReads,
                        int(std::chrono::duration_cast<std::chrono::hours>(runtime).count()),
                        int(std::chrono::duration_cast<std::chrono::minutes>(runtime).count()) % 60,
                        int(runtime.count()) % 60);
                std::cout << std::flush;

                if(progress < sequenceFileProperties.nReads){
                      std::this_thread::sleep_for(sleepinterval);
                      runtime = std::chrono::system_clock::now() - timepoint_begin;
                }
            }
        });

    TIMERSTARTCPU(correction);

        for (auto& thread : cpucorrectorThreads)
            thread.join();

        showProgress = false;
        progressThread.join();
        if(runtimeOptions.showProgress)
            printf("Progress: %3.2f %%\n", 100.00);

    TIMERSTOPCPU(correction);

        //std::cout << "threads done" << std::endl;

        minhasher.destroy();
    	readStorage.destroy();

        readIsProcessedVector.clear();
        readIsProcessedVector.shrink_to_fit();

        std::cout << "begin merge" << std::endl;
        TIMERSTARTCPU(merge);

        mergeResultFiles(sequenceFileProperties.nReads, fileOptions.inputfile, fileOptions.format, tmpfiles, fileOptions.outputfile);

        TIMERSTOPCPU(merge);

        deleteFiles(tmpfiles);

        std::vector<std::string> featureFiles(tmpfiles);
        for(auto& s : featureFiles)
            s = s + "_features";

        //concatenate feature files of each thread into one file

        if(correctionOptions.extractFeatures){
            std::stringstream commandbuilder;

            commandbuilder << "cat";

            for(const auto& featureFile : featureFiles){
                commandbuilder << " \"" << featureFile << "\"";
            }

            commandbuilder << " > \"" << fileOptions.outputfile << "_features\"";

            const std::string command = commandbuilder.str();
            TIMERSTARTCPU(concat_feature_files);
            int r1 = std::system(command.c_str());
            TIMERSTOPCPU(concat_feature_files);

            if(r1 != 0){
                std::cerr << "Warning. Feature files could not be concatenated!\n";
                std::cerr << "This command returned a non-zero error value: \n";
                std::cerr << command +  '\n';
                std::cerr << "Please concatenate the following files manually\n";
                for(const auto& s : featureFiles)
                    std::cerr << s << '\n';
            }else{
                deleteFiles(featureFiles);
            }
        }else{
            deleteFiles(featureFiles);
        }

        std::cout << "end merge" << std::endl;
    }


}
}

#endif