#include "../inc/correct.hpp"

#include "../inc/batchelem.hpp"
#include "../inc/graph.hpp"
#include "../inc/graphtools.hpp"
#include "../inc/hammingtools.hpp"
#include "../inc/pileup.hpp"
#include "../inc/read.hpp"
#include "../inc/sequencefileio.hpp"

#include <cstdint>
#include <thread>

//#define DO_PROFILE

#ifdef __NVCC__
#include <cuda_profiler_api.h>
#endif

namespace care{

/*
    Block distribution
*/
struct BatchGenerator{
    BatchGenerator(){}
    BatchGenerator(std::uint32_t firstId, std::uint32_t lastIdExcl, std::uint32_t batchsize)
            : batchsize(batchsize), firstId(firstId), lastIdExcl(lastIdExcl), currentId(firstId){
                if(batchsize == 0) throw std::runtime_error("BatchGenerator: invalid batch size");
                if(firstId >= lastIdExcl) throw std::runtime_error("BatchGenerator: firstId >= lastIdExcl");
            }
    BatchGenerator(std::uint32_t totalNumberOfReads, std::uint32_t batchsize_, int threadId, int nThreads){
        if(threadId < 0) throw std::runtime_error("BatchGenerator: invalid threadId");
        if(nThreads < 0) throw std::runtime_error("BatchGenerator: invalid nThreads");

    	std::uint32_t chunksize = totalNumberOfReads / nThreads;
    	int leftover = totalNumberOfReads % nThreads;

    	if(threadId < leftover){
    		chunksize++;
    		firstId = threadId == 0 ? 0 : threadId * chunksize;
    		lastIdExcl = firstId + chunksize;
    	}else{
    		firstId = leftover * (chunksize+1) + (threadId - leftover) * chunksize;;
    		lastIdExcl = firstId + chunksize;
    	}


        currentId = firstId;
        batchsize = batchsize_;
        //std::cout << "thread " << threadId << " firstId " << firstId << " lastIdExcl " << lastIdExcl << " batchsize " << batchsize << std::endl;
    };
    BatchGenerator(const BatchGenerator& rhs) = default;
    BatchGenerator(BatchGenerator&& rhs) = default;
    BatchGenerator& operator=(const BatchGenerator& rhs) = default;
    BatchGenerator& operator=(BatchGenerator&& rhs) = default;

    std::vector<std::uint32_t> getNextReadIds(){
        std::vector<std::uint32_t> result;
    	while(result.size() < batchsize && currentId < lastIdExcl){
    		result.push_back(currentId);
    		currentId++;
    	}
        return result;
    }
private:
    std::uint32_t batchsize;
    std::uint32_t firstId;
    std::uint32_t lastIdExcl;
    std::uint32_t currentId;
};



struct CorrectionThreadOptions{
    int threadId;
    int deviceId;

    std::string outputfile;
    BatchGenerator* batchGen;
    const Minhasher* minhasher;
    const ReadStorage* readStorage;
    std::mutex* coutLock;
    std::vector<char>* readIsProcessedVector;
    std::mutex* locksForProcessedFlags;
    std::size_t nLocksForProcessedFlags;
};

struct ErrorCorrectionThread{
    AlignmentOptions alignmentOptions;
    GoodAlignmentProperties goodAlignmentProperties;
    CorrectionOptions correctionOptions;
    CorrectionThreadOptions threadOpts;
    SequenceFileProperties fileProperties;

    std::uint32_t nProcessedReads = 0;

    DetermineGoodAlignmentStats goodAlignmentStats;
    std::uint64_t minhashcandidates = 0;
    std::uint64_t duplicates = 0;
	int nProcessedQueries = 0;
	int nCorrectedCandidates = 0; // candidates which were corrected in addition to query correction.

    int avgsupportfail = 0;
	int minsupportfail = 0;
	int mincoveragefail = 0;
	int sobadcouldnotcorrect = 0;
	int verygoodalignment = 0;

    std::chrono::duration<double> getCandidatesTimeTotal;
	std::chrono::duration<double> mapMinhashResultsToSequencesTimeTotal;
	std::chrono::duration<double> getAlignmentsTimeTotal;
	std::chrono::duration<double> determinegoodalignmentsTime;
	std::chrono::duration<double> fetchgoodcandidatesTime;
	std::chrono::duration<double> majorityvotetime;
	std::chrono::duration<double> basecorrectiontime;
	std::chrono::duration<double> readcorrectionTimeTotal;
	std::chrono::duration<double> mapminhashresultsdedup;
	std::chrono::duration<double> mapminhashresultsfetch;
	std::chrono::duration<double> graphbuildtime;
	std::chrono::duration<double> graphcorrectiontime;

    std::thread thread;
    bool isRunning = false;
    volatile bool stopAndAbort = false;

    void run(){
        if(isRunning) throw std::runtime_error("ErrorCorrectionThread::run: Is already running.");
        isRunning = true;
        thread = std::move(std::thread(&ErrorCorrectionThread::execute, this));
    }

    void join(){
        thread.join();
        isRunning = false;
    }
private:
    void execute();
};

void ErrorCorrectionThread::execute() {
    isRunning = true;

	std::chrono::time_point<std::chrono::system_clock> tpa, tpb, tpc, tpd;

	std::ofstream outputstream(threadOpts.outputfile);

	auto write_read = [&](const auto readId, const auto& sequence){
		auto& stream = outputstream;
		stream << readId << '\n';
		stream << sequence << '\n';
	};

    MinhasherBuffers minhasherbuffers(threadOpts.deviceId);

	hammingtools::SHDdata shddata(threadOpts.deviceId,
                                fileProperties.maxSequenceLength,
                                SDIV(fileProperties.maxSequenceLength, 4),
                                correctionOptions.batchsize);

	graphtools::AlignerDataArrays sgadata(threadOpts.deviceId, fileProperties.maxSequenceLength, alignmentOptions.alignmentscore_match,
			alignmentOptions.alignmentscore_sub, alignmentOptions.alignmentscore_ins, alignmentOptions.alignmentscore_del);


    hammingtools::correction::PileupImage pileupImage(correctionOptions.useQualityScores, correctionOptions.correctCandidates,
                                                        correctionOptions.estimatedCoverage, goodAlignmentProperties.max_mismatch_ratio,
                                                        correctionOptions.estimatedErrorrate, correctionOptions.m_coverage, correctionOptions.kmerlength);
    graphtools::correction::ErrorGraph errorgraph(correctionOptions.useQualityScores, goodAlignmentProperties.max_mismatch_ratio,
                                                  correctionOptions.graphalpha, correctionOptions.graphx);

    std::vector<BatchElem> batchElems;
    std::vector<std::uint32_t> readIds = threadOpts.batchGen->getNextReadIds();

    std::uint64_t numberOfBadAlignments = 0;

	while(!stopAndAbort &&!readIds.empty()){

		//fit vector size to actual batch size
		if (batchElems.size() != readIds.size()) {
            batchElems.resize(readIds.size(),
                              BatchElem(threadOpts.readStorage, correctionOptions.estimatedErrorrate,
                                        correctionOptions.estimatedCoverage, correctionOptions.m_coverage,
                                        goodAlignmentProperties.max_mismatch_ratio, goodAlignmentProperties.min_overlap,
                                        goodAlignmentProperties.min_overlap_ratio));
        }

        for(size_t i = 0; i < readIds.size(); i++){
            batchElems[i].set_read_id(readIds[i]);
            nProcessedQueries++;
        }

		if (correctionOptions.correctCandidates){
            for(auto& b : batchElems){
			    int batchlockindex = b.readId % threadOpts.nLocksForProcessedFlags;
			    std::unique_lock<std::mutex> lock(threadOpts.locksForProcessedFlags[batchlockindex]);
                if ((*threadOpts.readIsProcessedVector)[b.readId] == 0) {
					(*threadOpts.readIsProcessedVector)[b.readId] = 1;
				}else{
                    b.active = false;
                    nProcessedQueries--;
                }
            }
		}

        std::partition(batchElems.begin(), batchElems.end(), [](const auto& b){return b.active;});

        tpa = std::chrono::system_clock::now();

        for(auto& b : batchElems){
            if(b.active){
                b.fetch_query_data_from_readstorage();

                tpc = std::chrono::system_clock::now();
                b.set_candidate_ids(threadOpts.minhasher->getCandidates(minhasherbuffers, b.fwdSequenceString));
                tpd = std::chrono::system_clock::now();
        		getCandidatesTimeTotal += tpd - tpc;

                minhashcandidates += b.candidateIds.size();

				if(b.candidateIds.size() == 0){
					//no need for further processing
					b.active = false;
					write_read(b.readId, b.fwdSequenceString);
				}else{
                    b.make_unique_sequences();
                    duplicates += b.get_number_of_duplicate_sequences();
                    b.fetch_revcompl_sequences_from_readstorage();
                }
            }
        }

		tpb = std::chrono::system_clock::now();
		mapMinhashResultsToSequencesTimeTotal += tpb - tpa;

        tpa = std::chrono::system_clock::now();
        if (correctionOptions.correctionMode == CorrectionMode::Hamming) {
            hammingtools::getMultipleAlignments(shddata, batchElems, goodAlignmentProperties, true);
        }else if (correctionOptions.correctionMode == CorrectionMode::Graph){
            graphtools::getMultipleAlignments(sgadata, batchElems, true);
        }else{
            throw std::runtime_error("Alignment: invalid correction mode.");
        }

        tpb = std::chrono::system_clock::now();
        getAlignmentsTimeTotal += tpb - tpa;

        //select candidates from alignments
        for(auto& b : batchElems){
            if(b.active){
                tpc = std::chrono::system_clock::now();

                DetermineGoodAlignmentStats Astats = b.determine_good_alignments();

                if(Astats.correctionCases[3] > 0){
                    //no correction because not enough good alignments. write original sequence to output
                    write_read(b.readId, b.fwdSequenceString);
                }

                tpd = std::chrono::system_clock::now();
                determinegoodalignmentsTime += tpd - tpc;

                goodAlignmentStats.correctionCases[0] += Astats.correctionCases[0];
                goodAlignmentStats.correctionCases[1] += Astats.correctionCases[1];
                goodAlignmentStats.correctionCases[2] += Astats.correctionCases[2];
                goodAlignmentStats.correctionCases[3] += Astats.correctionCases[3];
                goodAlignmentStats.uniqueCandidatesWithoutGoodAlignment += Astats.uniqueCandidatesWithoutGoodAlignment;

                tpc = std::chrono::system_clock::now();

                if(b.active){
                    //move candidates which are used for correction to the front
                    b.prepare_good_candidates();
                }

                tpd = std::chrono::system_clock::now();
                fetchgoodcandidatesTime += tpd - tpc;
            }
        }

		if (correctionOptions.correctionMode == CorrectionMode::Hamming) {
            for(auto& b : batchElems){
                if(b.active){
                    tpc = std::chrono::system_clock::now();

                    pileupImage.correct_batch_elem(b);

                    tpd = std::chrono::system_clock::now();
                    readcorrectionTimeTotal += tpd - tpc;

                    majorityvotetime += pileupImage.timings.findconsensustime;
                    basecorrectiontime += pileupImage.timings.correctiontime;

                    avgsupportfail += pileupImage.properties.failedAvgSupport;
                    minsupportfail += pileupImage.properties.failedMinSupport;
                    mincoveragefail += pileupImage.properties.failedMinCoverage;
                    verygoodalignment += pileupImage.properties.isHQ;

                    if(b.corrected){
						write_read(b.readId, b.correctedSequence);
                    }else{
						write_read(b.readId, b.fwdSequenceString);
                    }

                    if (correctionOptions.correctCandidates) {
                        for(const auto& correctedCandidate : b.correctedCandidates){
                            const int count = b.candidateCountsPrefixSum[correctedCandidate.index+1]
                            - b.candidateCountsPrefixSum[correctedCandidate.index];
                            for(int f = 0; f < count; f++){
                                const int candidateId = b.candidateIds[count + f];
                                int batchlockindex = candidateId % threadOpts.nLocksForProcessedFlags;
                                bool savingIsOk = false;
                                if((*threadOpts.readIsProcessedVector)[candidateId] == 0){
                                    std::unique_lock <std::mutex> lock(
                                                    threadOpts.locksForProcessedFlags[batchlockindex]);
                                    if((*threadOpts.readIsProcessedVector)[candidateId]== 0) {
                                        (*threadOpts.readIsProcessedVector)[candidateId] = 1; // we will process this read
                                        lock.unlock();
                                        savingIsOk = true;
                                        nCorrectedCandidates++;
                                    }
                                }
                                if (savingIsOk) {
                                    if (b.bestIsForward[correctedCandidate.index])
										write_read(candidateId, correctedCandidate.sequence);
                                    else {
										//correctedCandidate.sequence is reverse complement, make reverse complement again
                                        const std::string fwd = SequenceGeneral(correctedCandidate.sequence, false).reverseComplement().toString();
                                        write_read(candidateId, fwd);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }else if (correctionOptions.correctionMode == CorrectionMode::Graph){

            for(auto& b : batchElems){
                if(b.active){
                    tpc = std::chrono::system_clock::now();

                    errorgraph.correct_batch_elem(b);

                    tpd = std::chrono::system_clock::now();
                    readcorrectionTimeTotal += tpd - tpc;

                    if(b.corrected){
						write_read(b.readId, b.correctedSequence);
                    }else{
						write_read(b.readId, b.fwdSequenceString);
                    }
                }
            }
        }else{
            throw std::runtime_error("Correction: invalid correction mode.");
        }

        for(auto& b : batchElems){
            if(b.active){
                numberOfBadAlignments += std::count_if(b.activeCandidates.begin(), b.activeCandidates.end(), [](bool b){return !b;});
            }
        }

		// update local progress
		nProcessedReads += readIds.size();

        readIds = threadOpts.batchGen->getNextReadIds();

	} // end batch processing

	{

		std::lock_guard < std::mutex > lg(*threadOpts.coutLock);
		std::cout << "thread " << threadOpts.threadId << " processed " << nProcessedQueries
				<< " queries" << std::endl;
		std::cout << "thread " << threadOpts.threadId << " corrected "
				<< nCorrectedCandidates << " candidates" << std::endl;
		std::cout << "thread " << threadOpts.threadId << " avgsupportfail "
				<< avgsupportfail << std::endl;
		std::cout << "thread " << threadOpts.threadId << " minsupportfail "
				<< minsupportfail << std::endl;
		std::cout << "thread " << threadOpts.threadId << " mincoveragefail "
				<< mincoveragefail << std::endl;
		std::cout << "thread " << threadOpts.threadId << " sobadcouldnotcorrect "
				<< sobadcouldnotcorrect << std::endl;
		std::cout << "thread " << threadOpts.threadId << " verygoodalignment "
				<< verygoodalignment << std::endl;
		std::cout << "thread " << threadOpts.threadId << " correctionCases "
				<< goodAlignmentStats.correctionCases[0] << " " << goodAlignmentStats.correctionCases[1] << " "
				<< goodAlignmentStats.correctionCases[2] << " " << goodAlignmentStats.correctionCases[3] << " "
				<< std::endl;
        std::cout << "thread " << threadOpts.threadId << " numberOfBadAlignments "
                << numberOfBadAlignments << std::endl;
        std::cout << "thread " << threadOpts.threadId
                << " : duplicates "
                << duplicates << " ( " << (100.0 * (double(duplicates) / double(minhashcandidates))) << " %)\n";
	}

#if 1
	{
        std::lock_guard < std::mutex > lg(*threadOpts.coutLock);
		if (correctionOptions.correctionMode == CorrectionMode::Hamming) {
			std::cout << "thread " << threadOpts.threadId << " : getCandidatesTimeTotal "
					<< getCandidatesTimeTotal.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : mapminhashresultsdedup "
					<< mapminhashresultsdedup.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : mapminhashresultsfetch "
					<< mapminhashresultsfetch.count() << '\n';
			std::cout << "thread " << threadOpts.threadId
					<< " : mapMinhashResultsToSequencesTimeTotal "
					<< mapMinhashResultsToSequencesTimeTotal.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment resize buffer "
					<< shddata.resizetime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment preprocessing "
					<< shddata.preprocessingtime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment H2D "
					<< shddata.h2dtime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment calculation "
					<< shddata.alignmenttime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment D2H "
					<< shddata.d2htime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment postprocessing "
					<< shddata.postprocessingtime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment total "
					<< getAlignmentsTimeTotal.count() << '\n';
			std::cout << "thread " << threadOpts.threadId
					<< " : correction find good alignments "
					<< determinegoodalignmentsTime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId
					<< " : correction fetch good data "
					<< fetchgoodcandidatesTime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : pileup vote "
					<< pileupImage.timings.findconsensustime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : pileup correct "
					<< pileupImage.timings.correctiontime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : correction calculation "
					<< readcorrectionTimeTotal.count() << '\n';
			// std::cout << "thread " << threadOpts.threadId << " : pileup resize buffer "
			// 		<< hcorrectionbuffers.resizetime.count() << '\n';
			// std::cout << "thread " << threadOpts.threadId << " : pileup preprocessing "
			// 		<< hcorrectionbuffers.preprocessingtime.count() << '\n';
			// std::cout << "thread " << threadOpts.threadId << " : pileup H2D "
			// 		<< hcorrectionbuffers.h2dtime.count() << '\n';
			// std::cout << "thread " << threadOpts.threadId << " : pileup calculation "
			// 		<< hcorrectionbuffers.correctiontime.count() << '\n';
			// std::cout << "thread " << threadOpts.threadId << " : pileup D2H "
			// 		<< hcorrectionbuffers.d2htime.count() << '\n';
			// std::cout << "thread " << threadOpts.threadId << " : pileup postprocessing "
			// 		<< hcorrectionbuffers.postprocessingtime.count() << '\n';
		} else if (correctionOptions.correctionMode == CorrectionMode::Graph) {
			std::cout << "thread " << threadOpts.threadId << " : getCandidatesTimeTotal "
					<< getCandidatesTimeTotal.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : mapminhashresultsdedup "
					<< mapminhashresultsdedup.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : mapminhashresultsfetch "
					<< mapminhashresultsfetch.count() << '\n';
			std::cout << "thread " << threadOpts.threadId
					<< " : mapMinhashResultsToSequencesTimeTotal "
					<< mapMinhashResultsToSequencesTimeTotal.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment total "
					<< getAlignmentsTimeTotal.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment resize buffer " << sgadata.resizetime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment preprocessing " << sgadata.preprocessingtime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment H2D " << sgadata.h2dtime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment calculation " << sgadata.alignmenttime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment D2H " << sgadata.d2htime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : alignment postprocessing " << sgadata.postprocessingtime.count() << '\n';
            std::cout << "thread " << threadOpts.threadId
					<< " : correction find good alignments "
					<< determinegoodalignmentsTime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId
					<< " : correction fetch good data "
					<< fetchgoodcandidatesTime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : graph build "
					<< graphbuildtime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : graph correct "
					<< graphcorrectiontime.count() << '\n';
			std::cout << "thread " << threadOpts.threadId << " : correction calculation "
					<< readcorrectionTimeTotal.count() << '\n';
		}
	}
#endif

	hammingtools::cuda_cleanup_SHDdata(shddata);
	graphtools::cuda_cleanup_AlignerDataArrays(sgadata);
	cuda_cleanup_MinhasherBuffers(minhasherbuffers);
}



void correct(const MinhashOptions& minhashOptions,
				  const AlignmentOptions& alignmentOptions,
				  const GoodAlignmentProperties& goodAlignmentProperties,
				  const CorrectionOptions& correctionOptions,
				  const RuntimeOptions& runtimeOptions,
				  const FileOptions& fileOptions,
                  Minhasher& minhasher,
                  ReadStorage& readStorage,
				  std::vector<char>& readIsProcessedVector,
				  std::unique_ptr<std::mutex[]>& locksForProcessedFlags,
				  size_t nLocksForProcessedFlags,
				  const std::vector<int>& deviceIds){

      // initialize global correction data structures
  	hammingtools::correction::init_once();
  	graphtools::correction::init_once();

    SequenceFileProperties props = getSequenceFileProperties(fileOptions.inputfile, fileOptions.format);

    std::cout << "min sequence length " << props.minSequenceLength << ", max sequence length " << props.maxSequenceLength << '\n';

    std::vector<std::string> tmpfiles;
    for(int i = 0; i < runtimeOptions.nCorrectorThreads; i++){
        tmpfiles.emplace_back(fileOptions.outputfile + "_tmp_" + std::to_string(i));
    }

    std::vector<BatchGenerator> generators(runtimeOptions.nCorrectorThreads);
    std::vector<ErrorCorrectionThread> ecthreads(runtimeOptions.nCorrectorThreads);

    std::mutex writelock;

#ifdef DO_PROFILE
#ifdef __NVCC__
    cudaProfilerStart(); CUERR;
#endif
#endif

    for(int threadId = 0; threadId < runtimeOptions.nCorrectorThreads; threadId++){

        generators[threadId] = BatchGenerator(props.nReads, correctionOptions.batchsize, threadId, runtimeOptions.nCorrectorThreads);
        CorrectionThreadOptions threadOpts;
        threadOpts.threadId = threadId;
        threadOpts.deviceId = deviceIds.size() == 0 ? -1 : deviceIds[threadId % deviceIds.size()];
        threadOpts.outputfile = tmpfiles[threadId];
        threadOpts.batchGen = &generators[threadId];
        threadOpts.minhasher = &minhasher;
        threadOpts.readStorage = &readStorage;
        threadOpts.coutLock = &writelock;
        threadOpts.readIsProcessedVector = &readIsProcessedVector;
        threadOpts.locksForProcessedFlags = locksForProcessedFlags.get();
        threadOpts.nLocksForProcessedFlags = nLocksForProcessedFlags;

        ecthreads[threadId].alignmentOptions = alignmentOptions;
        ecthreads[threadId].goodAlignmentProperties = goodAlignmentProperties;
        ecthreads[threadId].correctionOptions = correctionOptions;
        ecthreads[threadId].threadOpts = threadOpts;
        ecthreads[threadId].fileProperties = props;

        ecthreads[threadId].run();
    }

#ifdef DO_PROFILE
    int sleepiter = 0;
#endif
    if(runtimeOptions.showProgress){
        std::uint64_t progress = 0;
        while(progress < props.nReads){
            progress = 0;
            for(int threadId = 0; threadId < runtimeOptions.nCorrectorThreads; threadId++){
                progress += ecthreads[threadId].nProcessedReads;
            }
            printf("Progress: %3.2f %%\r",
                    ((progress * 1.0 / props.nReads) * 100.0));
            std::cout << std::flush;
            if(progress < props.nReads)
                  std::this_thread::sleep_for(std::chrono::seconds(3));
#ifdef DO_PROFILE
            sleepiter++;

            #ifdef __NVCC__
            if(sleepiter > 0){
                cudaProfilerStop(); CUERR;
                for(auto& t : ecthreads){
                    t.stopAndAbort = true;
                    t.join();
                }
                std::exit(0);
            }
            #endif
#endif
        }
    }

    for (auto& thread : ecthreads)
        thread.join();

    if(runtimeOptions.showProgress)
        printf("Progress: %3.2f %%\n", 100.00);

    minhasher.init(0);
	readStorage.destroy();

    std::cout << "begin merge" << std::endl;
    mergeResultFiles(props.nReads, fileOptions.inputfile, fileOptions.format, tmpfiles, fileOptions.outputfile);
    deleteFiles(tmpfiles);

    std::cout << "end merge" << std::endl;

}


}