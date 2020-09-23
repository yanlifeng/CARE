#ifndef CARE_CPU_MSA_HPP
#define CARE_CPU_MSA_HPP

#include <config.hpp>

#include <util.hpp>
#include <qualityscoreweights.hpp>
#include <bestalignment.hpp>
#include <string>
#include <cassert>
#include <vector>
#include <numeric>
#include <functional>
#include <algorithm>
#include <array>
#include <iostream>

namespace care{




struct MultipleSequenceAlignment{

public:

    struct InputData{
        bool useQualityScores;
        int subjectLength;
        int nCandidates;
        size_t candidatesPitch;
        size_t candidateQualitiesPitch;
        const char* subject;
        const char* candidates;
        const char* subjectQualities;
        const char* candidateQualities;
        const int* candidateLengths;
        const int* candidateShifts;
        const float* candidateDefaultWeightFactors;
    };

    struct PossibleMsaSplits{
        std::vector<std::vector<int>> splits;
    };

    std::vector<char> consensus;
    std::vector<float> support;
    std::vector<int> coverage;
    std::vector<float> origWeights;
    std::vector<int> origCoverages;

    std::vector<int> countsA;
    std::vector<int> countsC;
    std::vector<int> countsG;
    std::vector<int> countsT;

    std::vector<float> weightsA;
    std::vector<float> weightsC;
    std::vector<float> weightsG;
    std::vector<float> weightsT;

    std::vector<int> countsMatrixA;
    std::vector<int> countsMatrixC;
    std::vector<int> countsMatrixG;
    std::vector<int> countsMatrixT;

    int nCandidates{};
    int nColumns{};
    int addedSequences{};

    int subjectColumnsBegin_incl{};
    int subjectColumnsEnd_excl{};


    InputData inputData;


    void build(const InputData& args);

    void resize(int cols);

    void fillzero();

    void findConsensus();

    void findOrigWeightAndCoverage(const char* subject);

    void addSequence(bool useQualityScores, const char* sequence, const char* quality, int length, int shift, float defaultWeightFactor);

    //void removeSequence(bool useQualityScores, const char* sequence, const char* quality, int length, int shift, float defaultWeightFactor);

    void print(std::ostream& os) const;
    void printWithDiffToConsensus(std::ostream& os) const;

    void printCountMatrix(int which, std::ostream& os) const;

    PossibleMsaSplits inspectColumnsRegionSplit(int firstColumn) const;
    PossibleMsaSplits inspectColumnsRegionSplit(int firstColumn, int lastColumnExcl) const;
};

struct MSAProperties{
    float avg_support;
    float min_support;
    int max_coverage;
    int min_coverage;
    bool isHQ;
    bool failedAvgSupport;
    bool failedMinSupport;
    bool failedMinCoverage;
};

struct CorrectionResult{
    bool isCorrected;
    bool isHQ;
    std::string correctedSequence;
    std::vector<int> uncorrectedPositionsNoConsensus;
    std::vector<float> bestAlignmentWeightOfConsensusBase;
    std::vector<float> bestAlignmentWeightOfAnchorBase;

    void reset(){
        isCorrected = false;
        isHQ = false;
        correctedSequence.clear();
        uncorrectedPositionsNoConsensus.clear();
        bestAlignmentWeightOfConsensusBase.clear();
        bestAlignmentWeightOfAnchorBase.clear();
    }
};

struct CorrectedCandidate{
    int index;
    int shift;
    std::string sequence;
    CorrectedCandidate() noexcept{}
    CorrectedCandidate(int index, int s, const std::string& sequence) noexcept
        : index(index), shift(s), sequence(sequence){}
};

struct RegionSelectionResult{
    bool performedMinimization = false;
    std::vector<bool> differentRegionCandidate;

    int column = 0;
    char significantBase = 'F';
    char consensusBase = 'F';
    char originalBase = 'F';
    int significantCount = 0;
    int consensuscount = 0;
};

MSAProperties getMSAProperties(const float* support,
                            const int* coverage,
                            int nColumns,
                            float estimatedErrorrate,
                            float estimatedCoverage,
                            float m_coverage);

MSAProperties getMSAProperties2(const float* support,
                            const int* coverage,
                            int firstCol,
                            int lastCol, //exclusive
                            float estimatedErrorrate,
                            float estimatedCoverage,
                            float m_coverage);

CorrectionResult getCorrectedSubject(const char* consensus,
                                    const float* support,
                                    const int* coverage,
                                    const int* originalCoverage,
                                    int nColumns,
                                    const char* subject,
                                    bool isHQ,
                                    float estimatedErrorrate,
                                    float estimatedCoverage,
                                    float m_coverage,
                                    int neighborRegionSize);

//candidates is a 2d array of size candidatesPitch * nCandidates.
//candidates with reverse complement alignment must be reverse complement in this array.
CorrectionResult getCorrectedSubject(const char* consensus,
                                    const float* support,
                                    const int* coverage,
                                    const int* originalCoverage,
                                    int nColumns,
                                    const char* subject,
                                    int subjectColumnsBegin_incl,
                                    const char* candidates,
                                    int nCandidates,
                                    const float* candidateAlignmentWeights,
                                    const int* candidateLengths,
                                    const int* candidateShifts,
                                    size_t candidatesPitch,
                                    bool isHQ,
                                    float estimatedErrorrate,
                                    float estimatedCoverage,
                                    float m_coverage,
                                    int neighborRegionSize);


CorrectionResult getCorrectedSubjectNew(const char* consensus,
                                    const float* support,
                                    const int* coverage,
                                    const int* originalCoverage,
                                    int nColumns,
                                    const char* subject,
                                    int subjectColumnsBegin_incl,
                                    const char* candidates,
                                    int nCandidates,
                                    const float* candidateAlignmentWeights,
                                    const int* candidateLengths,
                                    const int* candidateShifts,
                                    size_t candidatesPitch,
                                    MSAProperties msaProperties,
                                    float estimatedErrorrate,
                                    float estimatedCoverage,
                                    float m_coverage,
                                    int neighborRegionSize,
                                    read_number readId);                                    

std::vector<CorrectedCandidate> getCorrectedCandidates(const char* consensus,
                                    const float* support,
                                    const int* coverage,
                                    int nColumns,
                                    int subjectColumnsBegin_incl,
                                    int subjectColumnsEnd_excl,
                                    const int* candidateShifts,
                                    const int* candidateLengths,
                                    int nCandidates,
                                    float estimatedErrorrate,
                                    float estimatedCoverage,
                                    float m_coverage,
                                    int new_columns_to_correct);



std::vector<CorrectedCandidate> getCorrectedCandidatesNew(const char* consensus,
                                    const float* support,
                                    const int* coverage,
                                    int nColumns,
                                    int subjectColumnsBegin_incl,
                                    int subjectColumnsEnd_excl,
                                    const int* candidateShifts,
                                    const int* candidateLengths,
                                    int nCandidates,
                                    float estimatedErrorrate,
                                    float estimatedCoverage,
                                    float m_coverage,
                                    int new_columns_to_correct);

RegionSelectionResult findCandidatesOfDifferentRegion(const char* subject,
                                                    int subjectLength,
                                                    const char* candidates,
                                                    const int* candidateLengths,
                                                    int nCandidates,
                                                    size_t candidatesPitch,
                                                    const char* consensus,
                                                    const int* countsA,
                                                    const int* countsC,
                                                    const int* countsG,
                                                    const int* countsT,
                                                    const float* weightsA,
                                                    const float* weightsC,
                                                    const float* weightsG,
                                                    const float* weightsT,
                                                    const int* alignments_nOps,
                                                    const int* alignments_overlaps,
                                                    int subjectColumnsBegin_incl,
                                                    int subjectColumnsEnd_excl,
                                                    const int* candidateShifts,
                                                    int dataset_coverage,
                                                    float desiredAlignmentMaxErrorRate);

std::pair<int,int> findGoodConsensusRegionOfSubject(const char* subject,
                                                    int subjectLength,
                                                    const char* consensus,
                                                    const int* candidateShifts,
                                                    const int* candidateLengths,
                                                    int nCandidates);

std::pair<int,int> findGoodConsensusRegionOfSubject2(const char* subject,
                                                    int subjectLength,
                                                    const int* coverage,
                                                    int nColumns,
                                                    int subjectColumnsEnd_excl);




extern cpu::QualityScoreConversion qualityConversion;


void printSequencesInMSA(std::ostream& out,
                         const char* subject,
                         int subjectLength,
                         const char* candidates,
                         const int* candidateLengths,
                         int nCandidates,
                         const int* candidateShifts,
                         int subjectColumnsBegin_incl,
                         int subjectColumnsEnd_excl,
                         int nColumns,
                         size_t candidatesPitch);

void printSequencesInMSAConsEq(std::ostream& out,
                      const char* subject,
                      int subjectLength,
                      const char* candidates,
                      const int* candidateLengths,
                      int nCandidates,
                      const int* candidateShifts,
                      const char* consensus,
                      int subjectColumnsBegin_incl,
                      int subjectColumnsEnd_excl,
                      int nColumns,
                      size_t candidatesPitch);


}





#endif
