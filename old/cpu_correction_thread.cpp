//#include <cpu_correction_thread.hpp>

#include <config.hpp>

#include "options.hpp"

#include <minhasher.hpp>
#include <readstorage.hpp>
#include "cpu_alignment.hpp"
#include "bestalignment.hpp"
#include <msa.hpp>
#include "qualityscoreweights.hpp"
#include "rangegenerator.hpp"
#include "featureextractor.hpp"
#include "forestclassifier.hpp"
#include "nn_classifier.hpp"

#include "cpu_correction_core.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#include <vector>

#include <omp.h>

#define USE_MSA_MINIMIZATION
//#define USE_SUBJECT_CLIPPING

#define ENABLE_TIMING

//#define PRINT_MSA

namespace care{
namespace cpu{

        struct CorrectionTask{
            CorrectionTask(){}

            CorrectionTask(read_number readId)
                :   active(true),
                    corrected(false),
                    readId(readId)
                    {}

            CorrectionTask(const CorrectionTask& other)
                : active(other.active),
                corrected(other.corrected),
                readId(other.readId),
                encodedSubjectPtr(other.encodedSubjectPtr),
                subjectQualityPtr(other.subjectQualityPtr),
                original_subject_string(other.original_subject_string),
                corrected_subject(other.corrected_subject),
                correctedCandidates(other.correctedCandidates),
                candidate_read_ids(other.candidate_read_ids){

            }

            CorrectionTask(CorrectionTask&& other){
                operator=(other);
            }

            CorrectionTask& operator=(const CorrectionTask& other){
                CorrectionTask tmp(other);
                swap(*this, tmp);
                return *this;
            }

            CorrectionTask& operator=(CorrectionTask&& other){
                swap(*this, other);
                return *this;
            }

            friend void swap(CorrectionTask& l, CorrectionTask& r) noexcept{
                using std::swap;

                swap(l.active, r.active);
                swap(l.corrected, r.corrected);
                swap(l.readId, r.readId);
                swap(l.encodedSubjectPtr, r.encodedSubjectPtr);
                swap(l.subjectQualityPtr, r.subjectQualityPtr);
                swap(l.original_subject_string, r.original_subject_string);
                swap(l.corrected_subject, r.corrected_subject);
                swap(l.correctedCandidates, r.correctedCandidates);
                swap(l.candidate_read_ids, r.candidate_read_ids);
            }

            bool active;
            bool corrected;
            read_number readId;
            const unsigned int* encodedSubjectPtr;
            const char* subjectQualityPtr;
            std::string original_subject_string;
            std::string corrected_subject;
            std::vector<CorrectedCandidate> correctedCandidates;
            std::vector<read_number> candidate_read_ids;
        };

        struct TaskData{
            MultipleSequenceAlignment multipleSequenceAlignment;
            MSAProperties msaProperties;

            std::vector<unsigned int> subjectsequence;
            std::vector<char> candidateData;
            std::vector<char> candidateRevcData;
            std::vector<int> candidateLengths;
            int max_candidate_length = 0;
            std::vector<char*> candidateDataPtrs;
            std::vector<char*> candidateRevcDataPtrs;

            std::vector<SHDResult> forwardAlignments;
            std::vector<SHDResult> revcAlignments;
            std::vector<BestAlignment_t> alignmentFlags;
            size_t numGoodAlignmentFlags;

            std::vector<SHDResult> bestAlignments;
            std::vector<BestAlignment_t> bestAlignmentFlags;
            std::vector<int> bestAlignmentShifts;
            std::vector<float> bestAlignmentWeights;
            std::vector<read_number> bestCandidateReadIds;
            std::vector<int> bestCandidateLengths;
            std::vector<char> bestCandidateData;
            std::vector<char*> bestCandidatePtrs;

            std::vector<char> bestCandidateQualityData;
            std::vector<char*> bestCandidateQualityPtrs;
            //std::vector<std::string> bestCandidateStrings;
            std::vector<char> bestCandidateStrings;

            std::vector<MSAFeature> msaforestfeatures;

            std::vector<CorrectedCandidate> correctedCandidates;
        };



        void getCandidates(CorrectionTask& task,
                            const Minhasher& minhasher,
                            int maxNumberOfCandidates,
                            int requiredHitsPerCandidate,
                            size_t maxNumResultsPerMapQuery){

            task.candidate_read_ids = minhasher.getCandidates(task.original_subject_string,
                                                               requiredHitsPerCandidate,
                                                               maxNumberOfCandidates,
                                                               maxNumResultsPerMapQuery);

            //remove our own read id from candidate list. candidate_read_ids is sorted.
            auto readIdPos = std::lower_bound(task.candidate_read_ids.begin(),
                                                task.candidate_read_ids.end(),
                                                task.readId);

            if(readIdPos != task.candidate_read_ids.end() && *readIdPos == task.readId){
                task.candidate_read_ids.erase(readIdPos);
            }
        }


        void getCandidateSequenceData(TaskData& data,
                                    const cpu::ContiguousReadStorage& readStorage,
                                    CorrectionTask& task,
                                    size_t encodedSequencePitch){

            const size_t numCandidates = task.candidate_read_ids.size();

            data.candidateLengths.clear();
            data.candidateLengths.reserve(numCandidates);

            for(const read_number candidateId : task.candidate_read_ids){
                const int candidateLength = readStorage.fetchSequenceLength(candidateId);
                data.candidateLengths.emplace_back(candidateLength);
            }

            //max_candidate_bytes = Sequence_t::getNumBytes(max_candidate_length);

            data.candidateData.clear();
            data.candidateData.resize(encodedSequencePitch * numCandidates, 0);
            data.candidateRevcData.clear();
            data.candidateRevcData.resize(encodedSequencePitch * numCandidates, 0);
            data.candidateDataPtrs.clear();
            data.candidateDataPtrs.resize(numCandidates, nullptr);
            data.candidateRevcDataPtrs.clear();
            data.candidateRevcDataPtrs.resize(numCandidates, nullptr);

            //copy candidate data and reverse complements into buffer

            constexpr size_t prefetch_distance_sequences = 4;

            for(size_t i = 0; i < numCandidates && i < prefetch_distance_sequences; ++i) {
                const read_number next_candidate_read_id = task.candidate_read_ids[i];
                const char* nextsequenceptr = readStorage.fetchSequenceData_ptr(next_candidate_read_id);
                __builtin_prefetch(nextsequenceptr, 0, 0);
            }

            for(size_t i = 0; i < numCandidates; i++){
                if(i + prefetch_distance_sequences < numCandidates) {
                    const read_number next_candidate_read_id = task.candidate_read_ids[i + prefetch_distance_sequences];
                    const char* nextsequenceptr = readStorage.fetchSequenceData_ptr(next_candidate_read_id);
                    __builtin_prefetch(nextsequenceptr, 0, 0);
                }

                const read_number candidateId = task.candidate_read_ids[i];
                const char* candidateptr = readStorage.fetchSequenceData_ptr(candidateId);
                const int candidateLength = data.candidateLengths[i];
                const int bytes = getEncodedNumInts2BitHiLo(candidateLength) * sizeof(unsigned int);

                char* const candidateDataBegin = data.candidateData.data() + i * encodedSequencePitch;
                char* const candidateRevcDataBegin = data.candidateRevcData.data() + i * encodedSequencePitch;

                std::copy(candidateptr, candidateptr + bytes, candidateDataBegin);
                reverseComplement2BitHiLo((unsigned int*)(candidateRevcDataBegin),
                                          (const unsigned int*)(candidateptr),
                                          candidateLength);

                data.candidateDataPtrs[i] = candidateDataBegin;
                data.candidateRevcDataPtrs[i] = candidateRevcDataBegin;
            }
        }


        void getCandidateAlignments(TaskData& data,
                                    CorrectionTask& task,
                                    size_t encodedSequencePitch,
                                    const GoodAlignmentProperties& alignmentProps){

            const std::size_t numCandidates = task.candidate_read_ids.size();

            data.forwardAlignments.resize(numCandidates);
            data.revcAlignments.resize(numCandidates);

            shd::cpu_multi_shifted_hamming_distance_popcount(data.forwardAlignments.begin(),
                                                            (const char*)task.encodedSubjectPtr,
                                                            task.original_subject_string.size(),
                                                            data.candidateData,
                                                            data.candidateLengths,
                                                            encodedSequencePitch,
                                                            alignmentProps.min_overlap,
                                                            alignmentProps.maxErrorRate,
                                                            alignmentProps.min_overlap_ratio);

            shd::cpu_multi_shifted_hamming_distance_popcount(data.revcAlignments.begin(),
                                                            (const char*)task.encodedSubjectPtr,
                                                            task.original_subject_string.size(),
                                                            data.candidateRevcData,
                                                            data.candidateLengths,
                                                            encodedSequencePitch,
                                                            alignmentProps.min_overlap,
                                                            alignmentProps.maxErrorRate,
                                                            alignmentProps.min_overlap_ratio);

            //decide whether to keep forward or reverse complement
            data.alignmentFlags = findBestAlignmentDirection(data.forwardAlignments,
                                                            data.revcAlignments,
                                                            task.original_subject_string.size(),
                                                            data.candidateLengths,
                                                            alignmentProps.min_overlap,
                                                            alignmentProps.maxErrorRate,
                                                            alignmentProps.min_overlap_ratio);

        }

        void gatherBestAlignmentData(TaskData& data,
                                  CorrectionTask& task,
                                  size_t encodedSequencePitch){

              data.bestAlignments.clear();
              data.bestAlignmentFlags.clear();
              data.bestCandidateReadIds.clear();
              data.bestCandidateData.clear();
              data.bestCandidateLengths.clear();
              data.bestCandidatePtrs.clear();

              data.bestAlignments.resize(data.numGoodAlignmentFlags);
              data.bestAlignmentFlags.resize(data.numGoodAlignmentFlags);
              data.bestCandidateReadIds.resize(data.numGoodAlignmentFlags);
              data.bestCandidateData.resize(data.numGoodAlignmentFlags * encodedSequencePitch);
              data.bestCandidateLengths.resize(data.numGoodAlignmentFlags);
              data.bestCandidatePtrs.resize(data.numGoodAlignmentFlags);

              for(size_t i = 0; i < data.numGoodAlignmentFlags; i++){
                  data.bestCandidatePtrs[i] = data.bestCandidateData.data() + i * encodedSequencePitch;
              }

              for(size_t i = 0, insertpos = 0; i < data.alignmentFlags.size(); i++){

                  const BestAlignment_t flag = data.alignmentFlags[i];
                  const auto& fwdAlignment = data.forwardAlignments[i];
                  const auto& revcAlignment = data.revcAlignments[i];
                  const read_number candidateId = task.candidate_read_ids[i];
                  const int candidateLength = data.candidateLengths[i];

                  if(flag == BestAlignment_t::Forward){
                      data.bestAlignmentFlags[insertpos] = flag;
                      data.bestCandidateReadIds[insertpos] = candidateId;
                      data.bestCandidateLengths[insertpos] = candidateLength;

                      data.bestAlignments[insertpos] = fwdAlignment;
                      std::copy(data.candidateDataPtrs[i],
                                data.candidateDataPtrs[i] + encodedSequencePitch,
                                data.bestCandidatePtrs[insertpos]);
                      insertpos++;
                  }else if(flag == BestAlignment_t::ReverseComplement){
                      data.bestAlignmentFlags[insertpos] = flag;
                      data.bestCandidateReadIds[insertpos] = candidateId;
                      data.bestCandidateLengths[insertpos] = candidateLength;

                      data.bestAlignments[insertpos] = revcAlignment;
                      std::copy(data.candidateRevcDataPtrs[i],
                                data.candidateRevcDataPtrs[i] + encodedSequencePitch,
                                data.bestCandidatePtrs[insertpos]);
                      insertpos++;
                  }else{
                      ;//BestAlignment_t::None discard alignment
                  }
              }
        }

        void filterBestAlignmentsByMismatchRatio(TaskData& data,
                  CorrectionTask& task,
                  size_t encodedSequencePitch,
                  const CorrectionOptions& correctionOptions,
                  const GoodAlignmentProperties& alignmentProps){
            //get indices of alignments which have a good mismatch ratio

            auto goodIndices = filterAlignmentsByMismatchRatio(data.bestAlignments,
                                                               correctionOptions.estimatedErrorrate,
                                                               correctionOptions.estimatedCoverage,
                                                               correctionOptions.m_coverage,
                                                               [hpc = correctionOptions.hits_per_candidate](){
                                                                   return hpc > 1;
                                                               });

            if(goodIndices.size() == 0){
                task.active = false; //no good mismatch ratio
            }else{
                //stream compaction. keep only data at positions given by goodIndices

                for(size_t i = 0; i < goodIndices.size(); i++){
                    const int fromIndex = goodIndices[i];
                    const int toIndex = i;

                    data.bestAlignments[toIndex] = data.bestAlignments[fromIndex];
                    data.bestAlignmentFlags[toIndex] = data.bestAlignmentFlags[fromIndex];
                    data.bestCandidateReadIds[toIndex] = data.bestCandidateReadIds[fromIndex];
                    data.bestCandidateLengths[toIndex] = data.bestCandidateLengths[fromIndex];

                    std::copy(data.bestCandidatePtrs[fromIndex],
                              data.bestCandidatePtrs[fromIndex] + encodedSequencePitch,
                              data.bestCandidatePtrs[toIndex]);
                }

                data.bestAlignments.erase(data.bestAlignments.begin() + goodIndices.size(),
                                     data.bestAlignments.end());
                data.bestAlignmentFlags.erase(data.bestAlignmentFlags.begin() + goodIndices.size(),
                                         data.bestAlignmentFlags.end());
                data.bestCandidateReadIds.erase(data.bestCandidateReadIds.begin() + goodIndices.size(),
                                           data.bestCandidateReadIds.end());
                data.bestCandidateLengths.erase(data.bestCandidateLengths.begin() + goodIndices.size(),
                                           data.bestCandidateLengths.end());
                data.bestCandidatePtrs.erase(data.bestCandidatePtrs.begin() + goodIndices.size(),
                                        data.bestCandidatePtrs.end());
                data.bestCandidateData.erase(data.bestCandidateData.begin() + goodIndices.size() * encodedSequencePitch,
                                        data.bestCandidateData.end());

                data.bestAlignmentShifts.resize(data.bestAlignments.size());
                data.bestAlignmentWeights.resize(data.bestAlignments.size());

                for(size_t i = 0; i < data.bestAlignments.size(); i++){
                    data.bestAlignmentShifts[i] = data.bestAlignments[i].shift;
                    data.bestAlignmentWeights[i] = 1.0f - std::sqrt(data.bestAlignments[i].nOps
                                                                        / (data.bestAlignments[i].overlap
                                                                            * alignmentProps.maxErrorRate));
                }
            }
        }



        void getCandidateQualities(TaskData& data,
                                    const cpu::ContiguousReadStorage& readStorage,
                                    CorrectionTask& task,
                                    int maximumSequenceLength){

            task.subjectQualityPtr = readStorage.fetchQuality_ptr(task.readId);

            data.bestCandidateQualityData.clear();
            data.bestCandidateQualityData.resize(maximumSequenceLength * data.bestAlignments.size());

            data.bestCandidateQualityPtrs.clear();
            data.bestCandidateQualityPtrs.resize(data.bestAlignments.size());

            constexpr size_t prefetch_distance_qualities = 4;

            for(size_t i = 0; i < data.bestAlignments.size() && i < prefetch_distance_qualities; ++i) {
                const read_number next_candidate_read_id = data.bestCandidateReadIds[i];
                const char* nextqualityptr = readStorage.fetchQuality_ptr(next_candidate_read_id);
                __builtin_prefetch(nextqualityptr, 0, 0);
            }

            for(size_t i = 0; i < data.bestAlignments.size(); i++){
                if(i + prefetch_distance_qualities < data.bestAlignments.size()) {
                    const read_number next_candidate_read_id = data.bestCandidateReadIds[i + prefetch_distance_qualities];
                    const char* nextqualityptr = readStorage.fetchQuality_ptr(next_candidate_read_id);
                    __builtin_prefetch(nextqualityptr, 0, 0);
                }
                const char* qualityptr = readStorage.fetchQuality_ptr(data.bestCandidateReadIds[i]);
                const int length = data.bestCandidateLengths[i];
                const BestAlignment_t flag = data.bestAlignmentFlags[i];

                if(flag == BestAlignment_t::Forward){
                    std::copy(qualityptr, qualityptr + length, &data.bestCandidateQualityData[maximumSequenceLength * i]);
                }else{
                    std::reverse_copy(qualityptr, qualityptr + length, &data.bestCandidateQualityData[maximumSequenceLength * i]);
                }
            }

            for(size_t i = 0; i < data.bestAlignments.size(); i++){
                data.bestCandidateQualityPtrs[i] = data.bestCandidateQualityData.data() + i * maximumSequenceLength;
            }
        }


        void makeCandidateStrings(TaskData& data,
                                    CorrectionTask& task,
                                    int maximumSequenceLength){
            data.bestCandidateStrings.clear();
            data.bestCandidateStrings.resize(data.bestAlignments.size() * maximumSequenceLength);

            for(size_t i = 0; i < data.bestAlignments.size(); i++){
                const char* ptr = data.bestCandidatePtrs[i];
                const int length = data.bestCandidateLengths[i];
                decode2BitHiLoSequence(&data.bestCandidateStrings[i * maximumSequenceLength],
                                        (const unsigned int*)ptr,
                                        length);
            }
        }

        void buildMultipleSequenceAlignment(TaskData& data,
                                            CorrectionTask& task,
                                            const CorrectionOptions& correctionOptions,
                                            int maximumSequenceLength){


            const char* const candidateQualityPtr = correctionOptions.useQualityScores ?
                                                    data.bestCandidateQualityData.data()
                                                    : nullptr;

            data.multipleSequenceAlignment.build(task.original_subject_string.c_str(),
                                            task.original_subject_string.length(),
                                            data.bestCandidateStrings.data(),
                                            data.bestCandidateLengths.data(),
                                            int(data.bestAlignments.size()),
                                            data.bestAlignmentShifts.data(),
                                            data.bestAlignmentWeights.data(),
                                            task.subjectQualityPtr,
                                            candidateQualityPtr,
                                            maximumSequenceLength,
                                            maximumSequenceLength,
                                            correctionOptions.useQualityScores);
        }

        void removeCandidatesOfDifferentRegionFromMSA(TaskData& data,
                                                        CorrectionTask& task,
                                                        const CorrectionOptions& correctionOptions,
                                                        size_t encodedSequencePitch,
                                                        int maximumSequenceLength){
            constexpr int max_num_minimizations = 5;

            if(max_num_minimizations > 0){
                int num_minimizations = 0;

                auto minimizationResult = findCandidatesOfDifferentRegion(task.original_subject_string.c_str(),
                                                                    task.original_subject_string.length(),
                                                                    data.bestCandidateStrings.data(),
                                                                    data.bestCandidateLengths.data(),
                                                                    int(data.bestAlignments.size()),
                                                                    maximumSequenceLength,
                                                                    data.multipleSequenceAlignment.consensus.data(),
                                                                    data.multipleSequenceAlignment.countsA.data(),
                                                                    data.multipleSequenceAlignment.countsC.data(),
                                                                    data.multipleSequenceAlignment.countsG.data(),
                                                                    data.multipleSequenceAlignment.countsT.data(),
                                                                    data.multipleSequenceAlignment.weightsA.data(),
                                                                    data.multipleSequenceAlignment.weightsC.data(),
                                                                    data.multipleSequenceAlignment.weightsG.data(),
                                                                    data.multipleSequenceAlignment.weightsT.data(),
                                                                    data.multipleSequenceAlignment.subjectColumnsBegin_incl,
                                                                    data.multipleSequenceAlignment.subjectColumnsEnd_excl,
                                                                    data.bestAlignmentShifts.data(),
                                                                    correctionOptions.estimatedCoverage);

                auto update_after_successfull_minimization = [&](){

                    if(minimizationResult.performedMinimization){
                        assert(minimizationResult.differentRegionCandidate.size() == data.bestAlignments.size());

                        bool anyRemoved = false;
                        size_t cur = 0;
                        for(size_t i = 0; i < minimizationResult.differentRegionCandidate.size(); i++){
                            if(!minimizationResult.differentRegionCandidate[i]){

                                data.bestAlignments[cur] = data.bestAlignments[i];
                                data.bestAlignmentShifts[cur] = data.bestAlignmentShifts[i];
                                data.bestAlignmentWeights[cur] = data.bestAlignmentWeights[i];
                                data.bestAlignmentFlags[cur] = data.bestAlignmentFlags[i];
                                data.bestCandidateReadIds[cur] = data.bestCandidateReadIds[i];
                                data.bestCandidateLengths[cur] = data.bestCandidateLengths[i];

                                std::copy(data.bestCandidateData.begin() + i * encodedSequencePitch,
                                        data.bestCandidateData.begin() + (i+1) * encodedSequencePitch,
                                        data.bestCandidateData.begin() + cur * encodedSequencePitch);
                                std::copy(data.bestCandidateQualityData.begin() + i * maximumSequenceLength,
                                        data.bestCandidateQualityData.begin() + (i+1) * maximumSequenceLength,
                                        data.bestCandidateQualityData.begin() + cur * maximumSequenceLength);
                                std::copy(data.bestCandidateStrings.begin() + i * maximumSequenceLength,
                                        data.bestCandidateStrings.begin() + (i+1) * maximumSequenceLength,
                                        data.bestCandidateStrings.begin() + cur * maximumSequenceLength);

                                cur++;

                            }else{
                                anyRemoved = true;
                            }
                        }

                        assert(anyRemoved);

                        data.bestAlignments.erase(data.bestAlignments.begin() + cur, data.bestAlignments.end());
                        data.bestAlignmentShifts.erase(data.bestAlignmentShifts.begin() + cur, data.bestAlignmentShifts.end());
                        data.bestAlignmentWeights.erase(data.bestAlignmentWeights.begin() + cur, data.bestAlignmentWeights.end());
                        data.bestAlignmentFlags.erase(data.bestAlignmentFlags.begin() + cur, data.bestAlignmentFlags.end());
                        data.bestCandidateReadIds.erase(data.bestCandidateReadIds.begin() + cur, data.bestCandidateReadIds.end());
                        data.bestCandidateLengths.erase(data.bestCandidateLengths.begin() + cur, data.bestCandidateLengths.end());

                        data.bestCandidateData.erase(data.bestCandidateData.begin() + cur * encodedSequencePitch, data.bestCandidateData.end());
                        data.bestCandidateQualityData.erase(data.bestCandidateQualityData.begin() + cur * maximumSequenceLength, data.bestCandidateQualityData.end());
                        data.bestCandidateStrings.erase(data.bestCandidateStrings.begin() + cur * maximumSequenceLength, data.bestCandidateStrings.end());

                        //build minimized multiple sequence alignment

                        buildMultipleSequenceAlignment(data,
                                                        task,
                                                        correctionOptions,
                                                        maximumSequenceLength);
                    }
                };

                update_after_successfull_minimization();


                num_minimizations++;

                while(num_minimizations <= max_num_minimizations
                        && minimizationResult.performedMinimization){

                    minimizationResult = findCandidatesOfDifferentRegion(task.original_subject_string.c_str(),
                                                                        task.original_subject_string.length(),
                                                                        data.bestCandidateStrings.data(),
                                                                        data.bestCandidateLengths.data(),
                                                                        int(data.bestAlignments.size()),
                                                                        maximumSequenceLength,
                                                                        data.multipleSequenceAlignment.consensus.data(),
                                                                        data.multipleSequenceAlignment.countsA.data(),
                                                                        data.multipleSequenceAlignment.countsC.data(),
                                                                        data.multipleSequenceAlignment.countsG.data(),
                                                                        data.multipleSequenceAlignment.countsT.data(),
                                                                        data.multipleSequenceAlignment.weightsA.data(),
                                                                        data.multipleSequenceAlignment.weightsC.data(),
                                                                        data.multipleSequenceAlignment.weightsG.data(),
                                                                        data.multipleSequenceAlignment.weightsT.data(),
                                                                        data.multipleSequenceAlignment.subjectColumnsBegin_incl,
                                                                        data.multipleSequenceAlignment.subjectColumnsEnd_excl,
                                                                        data.bestAlignmentShifts.data(),
                                                                        correctionOptions.estimatedCoverage);

                    update_after_successfull_minimization();

                    num_minimizations++;
                }
            }
        }




        void correctSubject(TaskData& data,
                            CorrectionTask& task,
                            const CorrectionOptions& correctionOptions,
                            int maximumSequenceLength){

            const int subjectColumnsBegin_incl = data.multipleSequenceAlignment.subjectColumnsBegin_incl;
            const int subjectColumnsEnd_excl = data.multipleSequenceAlignment.subjectColumnsEnd_excl;

            data.msaProperties = getMSAProperties2(data.multipleSequenceAlignment.support.data(),
                                                            data.multipleSequenceAlignment.coverage.data(),
                                                            subjectColumnsBegin_incl,
                                                            subjectColumnsEnd_excl,
                                                            correctionOptions.estimatedErrorrate,
                                                            correctionOptions.estimatedCoverage,
                                                            correctionOptions.m_coverage);

            auto correctionResult = getCorrectedSubject(data.multipleSequenceAlignment.consensus.data() + subjectColumnsBegin_incl,
                                                        data.multipleSequenceAlignment.support.data() + subjectColumnsBegin_incl,
                                                        data.multipleSequenceAlignment.coverage.data() + subjectColumnsBegin_incl,
                                                        data.multipleSequenceAlignment.origCoverages.data() + subjectColumnsBegin_incl,
                                                        int(task.original_subject_string.size()),
                                                        task.original_subject_string.c_str(),
                                                        subjectColumnsBegin_incl,
                                                        data.bestCandidateStrings.data(),
                                                        int(data.bestAlignmentWeights.size()),
                                                        data.bestAlignmentWeights.data(),
                                                        data.bestCandidateLengths.data(),
                                                        data.bestAlignmentShifts.data(),
                                                        maximumSequenceLength,
                                                        data.msaProperties.isHQ,
                                                        correctionOptions.estimatedErrorrate,
                                                        correctionOptions.estimatedCoverage,
                                                        correctionOptions.m_coverage,
                                                        correctionOptions.kmerlength);

            if(correctionResult.isCorrected){
                task.corrected_subject = correctionResult.correctedSequence;
                task.corrected = true;
            }
        }

        void correctCandidates(TaskData& data,
                                CorrectionTask& task,
                                const CorrectionOptions& correctionOptions){

            task.correctedCandidates = getCorrectedCandidates(data.multipleSequenceAlignment.consensus.data(),
                                                            data.multipleSequenceAlignment.support.data(),
                                                            data.multipleSequenceAlignment.coverage.data(),
                                                            data.multipleSequenceAlignment.nColumns,
                                                            data.multipleSequenceAlignment.subjectColumnsBegin_incl,
                                                            data.multipleSequenceAlignment.subjectColumnsEnd_excl,
                                                            data.bestAlignmentShifts.data(),
                                                            data.bestCandidateLengths.data(),
                                                            data.multipleSequenceAlignment.nCandidates,
                                                            correctionOptions.estimatedErrorrate,
                                                            correctionOptions.estimatedCoverage,
                                                            correctionOptions.m_coverage,
                                                            correctionOptions.new_columns_to_correct);
        }

        void correctSubjectWithForest(TaskData& data,
                                CorrectionTask& task,
                                const ForestClassifier& forestClassifier,
                                const CorrectionOptions& correctionOptions){

            auto MSAFeatures = extractFeatures(data.multipleSequenceAlignment.consensus.data(),
                                            data.multipleSequenceAlignment.support.data(),
                                            data.multipleSequenceAlignment.coverage.data(),
                                            data.multipleSequenceAlignment.origCoverages.data(),
                                            data.multipleSequenceAlignment.nColumns,
                                            data.multipleSequenceAlignment.subjectColumnsBegin_incl,
                                            data.multipleSequenceAlignment.subjectColumnsEnd_excl,
                                            task.original_subject_string,
                                            correctionOptions.kmerlength, 0.5f,
                                            correctionOptions.estimatedCoverage);

            task.corrected_subject = task.original_subject_string;

            for(const auto& msafeature : MSAFeatures){
                constexpr float maxgini = 0.05f;
                constexpr float forest_correction_fraction = 0.5f;

                const bool doCorrect = forestClassifier.shouldCorrect(
                                                msafeature.position_support,
                                                msafeature.position_coverage,
                                                msafeature.alignment_coverage,
                                                msafeature.dataset_coverage,
                                                msafeature.min_support,
                                                msafeature.min_coverage,
                                                msafeature.max_support,
                                                msafeature.max_coverage,
                                                msafeature.mean_support,
                                                msafeature.mean_coverage,
                                                msafeature.median_support,
                                                msafeature.median_coverage,
                                                maxgini,
                                                forest_correction_fraction);

                if(doCorrect){
                    task.corrected = true;

                    const int globalIndex = data.multipleSequenceAlignment.subjectColumnsBegin_incl + msafeature.position;
                    task.corrected_subject[msafeature.position] = data.multipleSequenceAlignment.consensus[globalIndex];
                }
            }
        }

        void correctSubjectWithNeuralNetwork(TaskData& data,
                                            CorrectionTask& task,
                                            const NN_Correction_Classifier& nnClassifier,
                                            const CorrectionOptions& correctionOptions){
            assert(false);
            /*auto MSAFeatures3 = extractFeatures3_2(
                                    data.multipleSequenceAlignment.countsA.data(),
                                    data.multipleSequenceAlignment.countsC.data(),
                                    data.multipleSequenceAlignment.countsG.data(),
                                    data.multipleSequenceAlignment.countsT.data(),
                                    data.multipleSequenceAlignment.weightsA.data(),
                                    data.multipleSequenceAlignment.weightsC.data(),
                                    data.multipleSequenceAlignment.weightsG.data(),
                                    data.multipleSequenceAlignment.weightsT.data(),
                                    data.multipleSequenceAlignment.nRows,
                                    data.multipleSequenceAlignment.columnProperties.columnsToCheck,
                                    data.multipleSequenceAlignment.consensus.data(),
                                    data.multipleSequenceAlignment.support.data(),
                                    data.multipleSequenceAlignment.coverage.data(),
                                    data.multipleSequenceAlignment.origCoverages.data(),
                                    data.multipleSequenceAlignment.columnProperties.subjectColumnsBegin_incl,
                                    data.multipleSequenceAlignment.columnProperties.subjectColumnsEnd_excl,
                                    task.original_subject_string,
                                    correctionOptions.estimatedCoverage);

                std::vector<float> predictions = nnClassifier.infer(MSAFeatures3);
                assert(predictions.size() == MSAFeatures3.size());

                task.corrected_subject = task.original_subject_string;

                for(size_t index = 0; index < predictions.size(); index++){
                    constexpr float threshold = 0.8;
                    const auto& msafeature = MSAFeatures3[index];

                    if(predictions[index] >= threshold){
                        task.corrected = true;

                        const int globalIndex = data.multipleSequenceAlignment.columnProperties.subjectColumnsBegin_incl + msafeature.position;
                        task.corrected_subject[msafeature.position] = data.multipleSequenceAlignment.consensus[globalIndex];
                    }
                }*/
        }


        void correct_cpu(const MinhashOptions& minhashOptions,
                          const AlignmentOptions& alignmentOptions,
                          const GoodAlignmentProperties& goodAlignmentProperties,
                          const CorrectionOptions& correctionOptions,
                          const RuntimeOptions& runtimeOptions,
                          const FileOptions& fileOptions,
                          const SequenceFileProperties& sequenceFileProperties,
                          Minhasher& minhasher,
                          cpu::ContiguousReadStorage& readStorage,
                          std::uint64_t maxCandidatesPerRead,
                          std::vector<char>& readIsCorrectedVector,
                          std::unique_ptr<std::mutex[]>& locksForProcessedFlags,
                          std::size_t nLocksForProcessedFlags){

            int oldNumOMPThreads = 1;
            #pragma omp parallel
            {
                #pragma omp single
                oldNumOMPThreads = omp_get_num_threads();
            }

            omp_set_num_threads(runtimeOptions.nCorrectorThreads);

            std::vector<std::string> tmpfiles{fileOptions.outputfile + "_tmp"};

            std::ofstream outputstream;

            outputstream = std::move(std::ofstream(tmpfiles[0]));
            if(!outputstream){
                throw std::runtime_error("Could not open output file " + tmpfiles[0]);
            }

            std::ofstream featurestream;
            //if(correctionOptions.extractFeatures){
                featurestream = std::move(std::ofstream(tmpfiles[0] + "_features"));
                if(!featurestream && correctionOptions.extractFeatures){
                    throw std::runtime_error("Could not open output feature file");
                }
            //}

            #ifndef DO_PROFILE
                    cpu::RangeGenerator<read_number> readIdGenerator(sequenceFileProperties.nReads);
            #else
                    cpu::RangeGenerator<read_number> readIdGenerator(num_reads_to_profile);
            #endif

            NN_Correction_Classifier_Base nnClassifierBase;
            NN_Correction_Classifier nnClassifier;
            if(correctionOptions.correctionType == CorrectionType::Convnet){
                nnClassifierBase = std::move(NN_Correction_Classifier_Base{"./nn_sources", fileOptions.nnmodelfilename});
                nnClassifier = std::move(NN_Correction_Classifier{&nnClassifierBase});
            }

            ForestClassifier forestClassifier;
            if(correctionOptions.correctionType == CorrectionType::Forest){
                forestClassifier = std::move(ForestClassifier{fileOptions.forestfilename});
            }

            auto write_read = [&](const read_number readId, const auto& sequence){
                //std::cout << readId << " " << sequence << std::endl;
                auto& stream = outputstream;
                assert(sequence.size() > 0);

                for(const auto& c : sequence){
                    assert(c == 'A' || c == 'C' || c == 'G' || c == 'T' || c =='N');
                }

                stream << readId << ' ' << sequence << '\n';
            };

            auto lock = [&](read_number readId){
                read_number index = readId % nLocksForProcessedFlags;
                locksForProcessedFlags[index].lock();
            };

            auto unlock = [&](read_number readId){
                read_number index = readId % nLocksForProcessedFlags;
                locksForProcessedFlags[index].unlock();
            };

            //auto identity = [](auto i){return i;};

            auto writeCorrectedSubjectToFile = [&](auto dataPerTask, auto correctionTasks){
                for(size_t i = 0; i < correctionTasks.size(); i++){
                    //auto& taskdata = dataPerTask[i];
                    auto& task = correctionTasks[i];

                    if(task.corrected){

                        write_read(task.readId, task.corrected_subject);
                        lock(task.readId);
                        readIsCorrectedVector[task.readId] = 1;
                        unlock(task.readId);
                    }else{

                        //make subject available for correction as a candidate
                        if(readIsCorrectedVector[task.readId] == 1){
                            lock(task.readId);
                            if(readIsCorrectedVector[task.readId] == 1){
                                readIsCorrectedVector[task.readId] = 0;
                            }
                            unlock(task.readId);
                        }
                    }
                }
            };


            std::chrono::time_point<std::chrono::system_clock> timepoint_begin = std::chrono::system_clock::now();
            std::chrono::duration<double> runtime = std::chrono::seconds(0);
            std::chrono::duration<double> getCandidatesTimeTotal{0};
            std::chrono::duration<double> copyCandidateDataToBufferTimeTotal{0};
            std::chrono::duration<double> getAlignmentsTimeTotal{0};
            std::chrono::duration<double> findBestAlignmentDirectionTimeTotal{0};
            std::chrono::duration<double> gatherBestAlignmentDataTimeTotal{0};
            std::chrono::duration<double> mismatchRatioFilteringTimeTotal{0};
            std::chrono::duration<double> compactBestAlignmentDataTimeTotal{0};
            std::chrono::duration<double> fetchQualitiesTimeTotal{0};
            std::chrono::duration<double> makeCandidateStringsTimeTotal{0};
            std::chrono::duration<double> msaAddSequencesTimeTotal{0};
            std::chrono::duration<double> msaFindConsensusTimeTotal{0};
            std::chrono::duration<double> msaMinimizationTimeTotal{0};
            std::chrono::duration<double> msaCorrectSubjectTimeTotal{0};
            std::chrono::duration<double> msaCorrectCandidatesTimeTotal{0};
            std::chrono::duration<double> correctWithFeaturesTimeTotal{0};


            const int encodedSequencePitch = sizeof(unsigned int) * getEncodedNumInts2BitHiLo(sequenceFileProperties.maxSequenceLength);

    		//std::chrono::time_point<std::chrono::system_clock> tpa, tpb, tpc, tpd;

            //const int numThreads = runtimeOptions.nCorrectorThreads;


            std::vector<TaskData> dataPerTask(correctionOptions.batchsize);


            std::vector<read_number> readIds;
            std::vector<CorrectionTask> correctionTasks;

            //std::cerr << "correctionOptions.hits_per_candidate " <<  correctionOptions.hits_per_candidate << ", max_candidates " << max_candidates << '\n';

    		while(!(readIdGenerator.empty() && readIds.empty())){

                if(readIds.empty()){
                    readIds = readIdGenerator.next_n(correctionOptions.batchsize);
                }
                if(readIds.empty()){
                    continue;
                }

                correctionTasks.resize(readIds.size());

                #pragma omp parallel for
                for(size_t i = 0; i < readIds.size(); i++){
                    auto& task = correctionTasks[i];

                    const read_number readId = readIds[i];
                    task = CorrectionTask(readId);

                    bool ok = false;
                    lock(readId);
                    if (readIsCorrectedVector[readId] == 0) {
                        readIsCorrectedVector[readId] = 1;
                        ok = true;
                    }else{
                    }
                    unlock(readId);

                    if(ok){
                        const char* originalsubjectptr = readStorage.fetchSequenceData_ptr(readId);
                        const int originalsubjectLength = readStorage.fetchSequenceLength(readId);

                        task.original_subject_string = get2BitHiLoString((const unsigned int*)originalsubjectptr, originalsubjectLength);

                        task.encodedSubjectPtr = (const unsigned int*) originalsubjectptr;
                    }else{
                        task.active = false;
                    }
                }

                correctionTasks.erase(std::remove_if(correctionTasks.begin(),
                                                    correctionTasks.end(),
                                                    [](const auto& task){
                                                        return !task.active;
                                                    }),
                                      correctionTasks.end());

                const size_t maxNumResultsPerMapQuery = correctionOptions.estimatedCoverage * 2.5;

#ifdef ENABLE_TIMING
                auto tpa = std::chrono::system_clock::now();
#endif

                #pragma omp parallel for
                for(size_t i = 0; i < correctionTasks.size(); i++){
                    auto& task = correctionTasks[i];
                    getCandidates(task,
                                  minhasher,
                                  maxCandidatesPerRead,
                                  correctionOptions.hits_per_candidate,
                                  maxNumResultsPerMapQuery);
                }

#ifdef ENABLE_TIMING
                getCandidatesTimeTotal += std::chrono::system_clock::now() - tpa;
#endif

                correctionTasks.erase(std::remove_if(correctionTasks.begin(),
                                                  correctionTasks.end(),
                                                  [](const auto& task){
                                                      return task.candidate_read_ids.empty();
                                                  }),
                                    correctionTasks.end());

#ifdef ENABLE_TIMING
                tpa = std::chrono::system_clock::now();
#endif

                #pragma omp parallel for
                for(size_t i = 0; i < correctionTasks.size(); i++){
                    auto& taskdata = dataPerTask[i];
                    auto& task = correctionTasks[i];
                    getCandidateSequenceData(taskdata, readStorage, task, encodedSequencePitch);
                }

#ifdef ENABLE_TIMING
                copyCandidateDataToBufferTimeTotal += std::chrono::system_clock::now() - tpa;
#endif

#ifdef ENABLE_TIMING
                tpa = std::chrono::system_clock::now();
#endif

                #pragma omp parallel for
                for(size_t i = 0; i < correctionTasks.size(); i++){
                    auto& taskdata = dataPerTask[i];
                    auto& task = correctionTasks[i];

                    getCandidateAlignments(taskdata, task, encodedSequencePitch, goodAlignmentProperties);

                    taskdata.numGoodAlignmentFlags = std::count_if(taskdata.alignmentFlags.begin(),
                                                                 taskdata.alignmentFlags.end(),
                                                                 [](const auto flag){
                                                                    return flag != BestAlignment_t::None;
                                                                 });

                    if(taskdata.numGoodAlignmentFlags == 0){
                        task.active = false; //no good alignments
                    }
                }


#ifdef ENABLE_TIMING
                getAlignmentsTimeTotal += std::chrono::system_clock::now() - tpa;
#endif

                correctionTasks.erase(std::remove_if(correctionTasks.begin(),
                                                    correctionTasks.end(),
                                                    [](const auto& task){
                                                        return !task.active;
                                                    }),
                                      correctionTasks.end());


#ifdef ENABLE_TIMING
                tpa = std::chrono::system_clock::now();
#endif

                //gather data for candidates with good alignment direction

                #pragma omp parallel for
                for(size_t i = 0; i < correctionTasks.size(); i++){
                    auto& taskdata = dataPerTask[i];
                    auto& task = correctionTasks[i];

                    gatherBestAlignmentData(taskdata, task, encodedSequencePitch);
                }

#ifdef ENABLE_TIMING
                gatherBestAlignmentDataTimeTotal += std::chrono::system_clock::now() - tpa;
#endif



#ifdef ENABLE_TIMING
                tpa = std::chrono::system_clock::now();
#endif

                #pragma omp parallel for
                for(size_t i = 0; i < correctionTasks.size(); i++){
                    auto& taskdata = dataPerTask[i];
                    auto& task = correctionTasks[i];

                    filterBestAlignmentsByMismatchRatio(taskdata,
                                                      task,
                                                      encodedSequencePitch,
                                                      correctionOptions,
                                                      goodAlignmentProperties);
                }

#ifdef ENABLE_TIMING
                mismatchRatioFilteringTimeTotal += std::chrono::system_clock::now() - tpa;
#endif


                correctionTasks.erase(std::remove_if(correctionTasks.begin(),
                                                    correctionTasks.end(),
                                                    [](const auto& task){
                                                        return !task.active;
                                                    }),
                                      correctionTasks.end());


#ifdef ENABLE_TIMING
                    tpa = std::chrono::system_clock::now();
#endif


                    if(correctionOptions.useQualityScores){
                        //gather quality scores of best alignments

                        #pragma omp parallel for
                        for(size_t i = 0; i < correctionTasks.size(); i++){
                            auto& taskdata = dataPerTask[i];
                            auto& task = correctionTasks[i];

                            getCandidateQualities(taskdata,
                                                readStorage,
                                                task,
                                                sequenceFileProperties.maxSequenceLength);
                        }
                    }
#ifdef ENABLE_TIMING
                    fetchQualitiesTimeTotal += std::chrono::system_clock::now() - tpa;
#endif


#ifdef ENABLE_TIMING
                    tpa = std::chrono::system_clock::now();
#endif


                    #pragma omp parallel for
                    for(size_t i = 0; i < correctionTasks.size(); i++){
                        auto& taskdata = dataPerTask[i];
                        auto& task = correctionTasks[i];

                        makeCandidateStrings(taskdata,
                                            task,
                                            sequenceFileProperties.maxSequenceLength);
                    }



#ifdef ENABLE_TIMING
                    makeCandidateStringsTimeTotal += std::chrono::system_clock::now() - tpa;
#endif



#ifdef ENABLE_TIMING
                    tpa = std::chrono::system_clock::now();
#endif

                    #pragma omp parallel for
                    for(size_t i = 0; i < correctionTasks.size(); i++){
                        auto& taskdata = dataPerTask[i];
                        auto& task = correctionTasks[i];

                        buildMultipleSequenceAlignment(taskdata,
                                                       task,
                                                       correctionOptions,
                                                       sequenceFileProperties.maxSequenceLength);
                    }






#ifdef ENABLE_TIMING
                    msaFindConsensusTimeTotal += std::chrono::system_clock::now() - tpa;
#endif



#ifdef USE_MSA_MINIMIZATION

#ifdef ENABLE_TIMING
                    tpa = std::chrono::system_clock::now();
#endif


#ifdef PRINT_MSA
                    std::cout << correctionTasks[0].readId << " MSA: rows = " << (int(bestAlignments.size()) + 1) << " columns = " << multipleSequenceAlignment.nColumns << "\n";
                    std::cout << "Consensus:\n   ";
                    for(int i = 0; i < multipleSequenceAlignment.nColumns; i++){
                        std::cout << multipleSequenceAlignment.consensus[i];
                    }
                    std::cout << '\n';

                    /*printSequencesInMSA(std::cout,
                                        correctionTasks[0].original_subject_string.c_str(),
                                        subjectLength,
                                        bestCandidateStrings.data(),
                                        bestCandidateLengths.data(),
                                        int(bestAlignments.size()),
                                        bestAlignmentShifts.data(),
                                        multipleSequenceAlignment.subjectColumnsBegin_incl,
                                        multipleSequenceAlignment.subjectColumnsEnd_excl,
                                        multipleSequenceAlignment.nColumns,
                                        sequenceFileProperties.maxSequenceLength);*/

                    printSequencesInMSAConsEq(std::cout,
                                        correctionTasks[0].original_subject_string.c_str(),
                                        subjectLength,
                                        bestCandidateStrings.data(),
                                        bestCandidateLengths.data(),
                                        int(bestAlignments.size()),
                                        bestAlignmentShifts.data(),
                                        multipleSequenceAlignment.consensus.data(),
                                        multipleSequenceAlignment.subjectColumnsBegin_incl,
                                        multipleSequenceAlignment.subjectColumnsEnd_excl,
                                        multipleSequenceAlignment.nColumns,
                                        sequenceFileProperties.maxSequenceLength);
#endif

                    #pragma omp parallel for
                    for(size_t i = 0; i < correctionTasks.size(); i++){
                        auto& taskdata = dataPerTask[i];
                        auto& task = correctionTasks[i];

                        removeCandidatesOfDifferentRegionFromMSA(taskdata,
                                                                task,
                                                                correctionOptions,
                                                                encodedSequencePitch,
                                                                sequenceFileProperties.maxSequenceLength);
                    }


#ifdef ENABLE_TIMING
                    msaMinimizationTimeTotal += std::chrono::system_clock::now() - tpa;
#endif

#endif // USE_MSA_MINIMIZATION


                if(correctionOptions.extractFeatures){

                    #pragma omp parallel for
                    for(size_t i = 0; i < correctionTasks.size(); i++){
                        auto& taskdata = dataPerTask[i];
                        auto& task = correctionTasks[i];

                        #if 1
                        taskdata.msaforestfeatures = extractFeatures(taskdata.multipleSequenceAlignment.consensus.data(),
                                                                    taskdata.multipleSequenceAlignment.support.data(),
                                                                    taskdata.multipleSequenceAlignment.coverage.data(),
                                                                    taskdata.multipleSequenceAlignment.origCoverages.data(),
                                                                    taskdata.multipleSequenceAlignment.nColumns,
                                                                    taskdata.multipleSequenceAlignment.subjectColumnsBegin_incl,
                                                                    taskdata.multipleSequenceAlignment.subjectColumnsEnd_excl,
                                                                    task.original_subject_string,
                                                                    correctionOptions.kmerlength, 0.5f,
                                                                    correctionOptions.estimatedCoverage);
                        #else

                        auto MSAFeatures = extractFeatures3_2(
                                                    multipleSequenceAlignment.countsA.data(),
                                                    multipleSequenceAlignment.countsC.data(),
                                                    multipleSequenceAlignment.countsG.data(),
                                                    multipleSequenceAlignment.countsT.data(),
                                                    multipleSequenceAlignment.weightsA.data(),
                                                    multipleSequenceAlignment.weightsC.data(),
                                                    multipleSequenceAlignment.weightsG.data(),
                                                    multipleSequenceAlignment.weightsT.data(),
                                                    multipleSequenceAlignment.nCandidates+1,
                                                    multipleSequenceAlignment.nColumns,
                                                    multipleSequenceAlignment.consensus.data(),
                                                    multipleSequenceAlignment.support.data(),
                                                    multipleSequenceAlignment.coverage.data(),
                                                    multipleSequenceAlignment.origCoverages.data(),
                                                    multipleSequenceAlignment.subjectColumnsBegin_incl,
                                                    multipleSequenceAlignment.subjectColumnsEnd_excl,
                                                    task.original_subject_string,
                                                    correctionOptions.estimatedCoverage);
                        #endif
                    }

                    for(size_t i = 0; i < correctionTasks.size(); i++){
                        auto& taskdata = dataPerTask[i];
                        auto& task = correctionTasks[i];

                        for(const auto& msafeature : taskdata.msaforestfeatures){
                            featurestream << task.readId << '\t' << msafeature.position << '\t' << msafeature.consensus << '\n';
                            featurestream << msafeature << '\n';
                        }
                    }



                }else{ //correction is not performed when extracting features

                    if(correctionOptions.correctionType == CorrectionType::Classic){

    #ifdef ENABLE_TIMING
                        auto tpa = std::chrono::system_clock::now();
    #endif

                        #pragma omp parallel for
                        for(size_t i = 0; i < correctionTasks.size(); i++){
                            auto& taskdata = dataPerTask[i];
                            auto& task = correctionTasks[i];

                            correctSubject(taskdata,
                                            task,
                                            correctionOptions,
                                            sequenceFileProperties.maxSequenceLength);
                        }


    #ifdef ENABLE_TIMING
                        msaCorrectSubjectTimeTotal += std::chrono::system_clock::now() - tpa;
    #endif

                        writeCorrectedSubjectToFile(dataPerTask, correctionTasks);



                        //get corrected candidates and write them to file
                        if(correctionOptions.correctCandidates){
    #ifdef ENABLE_TIMING
                            tpa = std::chrono::system_clock::now();
    #endif

                            #pragma omp parallel for
                            for(size_t i = 0; i < correctionTasks.size(); i++){
                                auto& taskdata = dataPerTask[i];
                                auto& task = correctionTasks[i];

                                if(taskdata.msaProperties.isHQ){
                                    correctCandidates(taskdata, task, correctionOptions);
                                }
                            }

    #ifdef ENABLE_TIMING
                            msaCorrectCandidatesTimeTotal += std::chrono::system_clock::now() - tpa;
    #endif

                            for(size_t i = 0; i < correctionTasks.size(); i++){
                                auto& taskdata = dataPerTask[i];
                                auto& task = correctionTasks[i];

                                for(const auto& correctedCandidate : task.correctedCandidates){
                                    const read_number candidateId = taskdata.bestCandidateReadIds[correctedCandidate.index];
                                    bool savingIsOk = false;
                                    if(readIsCorrectedVector[candidateId] == 0){
                                        lock(candidateId);
                                        if(readIsCorrectedVector[candidateId]== 0) {
                                            readIsCorrectedVector[candidateId] = 1; // we will process this read
                                            savingIsOk = true;
                                        }
                                        unlock(candidateId);
                                    }

                                    if (savingIsOk) {
                                        if(taskdata.bestAlignmentFlags[correctedCandidate.index] == BestAlignment_t::Forward){
                                            write_read(candidateId, correctedCandidate.sequence);
                                        }else{
                                            std::string fwd;
                                            fwd.resize(correctedCandidate.sequence.length());
                                            reverseComplementString(&fwd[0], correctedCandidate.sequence.c_str(), correctedCandidate.sequence.length());
                                            write_read(candidateId, fwd);
                                        }
                                    }
                                }

                            }
                        }

                    }else{

    #ifdef ENABLE_TIMING
                        auto tpa = std::chrono::system_clock::now();
    #endif

                        #pragma omp parallel for
                        for(size_t i = 0; i < correctionTasks.size(); i++){
                            auto& taskdata = dataPerTask[i];
                            auto& task = correctionTasks[i];
#if 1
                            correctSubjectWithForest(taskdata, task, forestClassifier, correctionOptions);
#else
                            correctSubjectWithNeuralNetwork(taskdata, task, nnClassifier, correctionOptions);
#endif
                        }

    #ifdef ENABLE_TIMING
                        correctWithFeaturesTimeTotal += std::chrono::system_clock::now() - tpa;
    #endif


                        writeCorrectedSubjectToFile(dataPerTask, correctionTasks);

                    }
                }

                if(runtimeOptions.showProgress/* && readIdGenerator.getCurrentUnsafe() - previousprocessedreads > 100000*/){
                    const auto now = std::chrono::system_clock::now();
                    runtime = now - timepoint_begin;

                    printf("Processed %10u of %10lu reads (Runtime: %03d:%02d:%02d)\r",
                            readIdGenerator.getCurrentUnsafe() - readIdGenerator.getBegin(), sequenceFileProperties.nReads,
                            int(std::chrono::duration_cast<std::chrono::hours>(runtime).count()),
                            int(std::chrono::duration_cast<std::chrono::minutes>(runtime).count()) % 60,
                            int(runtime.count()) % 60);
                    //previousprocessedreads = readIdGenerator.getCurrentUnsafe();
                }


    		} // end batch processing

            if(runtimeOptions.showProgress/* && readIdGenerator.getCurrentUnsafe() - previousprocessedreads > 100000*/){
                const auto now = std::chrono::system_clock::now();
                runtime = now - timepoint_begin;

                printf("Processed %10u of %10lu reads (Runtime: %03d:%02d:%02d)\r",
                        readIdGenerator.getCurrentUnsafe() - readIdGenerator.getBegin(), sequenceFileProperties.nReads,
                        int(std::chrono::duration_cast<std::chrono::hours>(runtime).count()),
                        int(std::chrono::duration_cast<std::chrono::minutes>(runtime).count()) % 60,
                        int(runtime.count()) % 60);
                //previousprocessedreads = readIdGenerator.getCurrentUnsafe();
            }

            featurestream.flush();
            outputstream.flush();

            minhasher.destroy();
        	readStorage.destroy();


            std::chrono::duration<double> totalDuration = getCandidatesTimeTotal
                                                        + copyCandidateDataToBufferTimeTotal
                                                        + getAlignmentsTimeTotal
                                                        + findBestAlignmentDirectionTimeTotal
                                                        + gatherBestAlignmentDataTimeTotal
                                                        + mismatchRatioFilteringTimeTotal
                                                        + compactBestAlignmentDataTimeTotal
                                                        + fetchQualitiesTimeTotal
                                                        + makeCandidateStringsTimeTotal
                                                        + msaAddSequencesTimeTotal
                                                        + msaFindConsensusTimeTotal
                                                        + msaMinimizationTimeTotal
                                                        + msaCorrectSubjectTimeTotal
                                                        + msaCorrectCandidatesTimeTotal
                                                        + correctWithFeaturesTimeTotal;

            auto printDuration = [&](const auto& name, const auto& duration){
                std::cout << "# elapsed time ("<< name << "): "
                          << duration.count()  << " s. "
                          << (100.0 * duration / totalDuration) << " %."<< std::endl;
            };

            #define printme(x) printDuration((#x),(x));

            printme(getCandidatesTimeTotal);
            printme(copyCandidateDataToBufferTimeTotal);
            printme(getAlignmentsTimeTotal);
            printme(findBestAlignmentDirectionTimeTotal);
            printme(gatherBestAlignmentDataTimeTotal);
            printme(mismatchRatioFilteringTimeTotal);
            printme(compactBestAlignmentDataTimeTotal);
            printme(fetchQualitiesTimeTotal);
            printme(makeCandidateStringsTimeTotal);
            printme(msaAddSequencesTimeTotal);
            printme(msaFindConsensusTimeTotal);
            printme(msaMinimizationTimeTotal);
            printme(msaCorrectSubjectTimeTotal);
            printme(msaCorrectCandidatesTimeTotal);
            printme(correctWithFeaturesTimeTotal);

            #undef printme

            std::cout << "Correction finished. Constructing result file." << std::endl;

            if(!correctionOptions.extractFeatures){

                std::cout << "begin merging reads" << std::endl;

                TIMERSTARTCPU(merge);

                mergeResultFiles(sequenceFileProperties.nReads, fileOptions.inputfile, fileOptions.format, tmpfiles, fileOptions.outputfile);

                TIMERSTOPCPU(merge);

                std::cout << "end merging reads" << std::endl;

            }

            deleteFiles(tmpfiles);

            std::vector<std::string> featureFiles(tmpfiles);
            for(auto& s : featureFiles)
                s = s + "_features";

            //concatenate feature files of each thread into one file

            if(correctionOptions.extractFeatures){
                std::cout << "begin merging features" << std::endl;

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

                std::cout << "end merging features" << std::endl;
            }else{
                deleteFiles(featureFiles);
            }

            std::cout << "end merge" << std::endl;

            omp_set_num_threads(oldNumOMPThreads);
    	}

}
}


#ifdef MSA_IMPLICIT
#undef MSA_IMPLICIT
#endif

#ifdef ENABLE_TIMING
#undef ENABLE_TIMING
#endif