#include <cpu_alignment.hpp>
#include <config.hpp>

#include <sequence.hpp>

#include <vector>


namespace care{
namespace cpu{
namespace shd{

    AlignmentResult
    cpu_shifted_hamming_distance_popcount(const char* subject,
                                int subjectLength,
                                const char* query,
                                int queryLength,
                                int min_overlap,
                                float maxErrorRate,
                                float min_overlap_ratio) noexcept{

        auto getNumBytes = [] (int sequencelength){
            return Sequence2BitHiLo::getNumBytes(sequencelength);
        };

        auto popcount = [](auto i){return __builtin_popcount(i);};

        auto identity = [](auto i){return i;};

        const int subjectbytes = getNumBytes(subjectLength);
        const int querybytes = getNumBytes(queryLength);
        const int subjectInts = subjectbytes / sizeof(unsigned int);
        const int queryInts = querybytes / sizeof(unsigned int);
        const int totalbases = subjectLength + queryLength;
        const int minoverlap = std::max(min_overlap, int(float(subjectLength) * min_overlap_ratio));

        int bestScore = totalbases; // score is number of mismatches
        int bestShift = -queryLength; // shift of query relative to subject. shift < 0 if query begins before subject

        std::vector<char> subjectdata(subjectbytes);
        std::vector<char> querydata(querybytes);

        unsigned int* subjectdata_hi = (unsigned int*)subjectdata.data();
        unsigned int* subjectdata_lo = (unsigned int*)(subjectdata.data() + subjectbytes / 2);
        unsigned int* querydata_hi = (unsigned int*)querydata.data();
        unsigned int* querydata_lo = (unsigned int*)(querydata.data() + querybytes / 2);

        std::copy(subject, subject + subjectbytes, subjectdata.begin());
        std::copy(query, query + querybytes, querydata.begin());

        auto hammingDistanceWithShift = [&](int shift, int overlapsize, int max_errors,
                                    unsigned int* shiftptr_hi, unsigned int* shiftptr_lo, auto transfunc1,
                                    int shiftptr_size,
                                    const unsigned int* otherptr_hi, const unsigned int* otherptr_lo,
                                    auto transfunc2){

            const int shiftamount = shift == 0 ? 0 : 1;

            shiftBitArrayLeftBy(shiftptr_hi, shiftptr_size / 2, shiftamount, transfunc1);
            shiftBitArrayLeftBy(shiftptr_lo, shiftptr_size / 2, shiftamount, transfunc1);

            const int score = hammingdistanceHiLo(shiftptr_hi,
                                                shiftptr_lo,
                                                otherptr_hi,
                                                otherptr_lo,
                                                overlapsize,
                                                overlapsize,
                                                max_errors,
                                                transfunc1,
                                                transfunc2,
                                                popcount);

            return score;
        };

        auto handle_shift = [&](int shift, int overlapsize,
                                    unsigned int* shiftptr_hi, unsigned int* shiftptr_lo, auto transfunc1,
                                    int shiftptr_size,
                                    const unsigned int* otherptr_hi, const unsigned int* otherptr_lo,
                                    auto transfunc2){

            const int max_errors = int(float(overlapsize) * maxErrorRate);

            int score = hammingDistanceWithShift(shift, overlapsize, max_errors,
                                shiftptr_hi,shiftptr_lo, transfunc1,
                                shiftptr_size,
                                otherptr_hi, otherptr_lo, transfunc2);

            score = (score < max_errors ?
                    score + totalbases - 2*overlapsize // non-overlapping regions count as mismatches
                    : std::numeric_limits<int>::max()); // too many errors, discard

            if(score < bestScore){
                bestScore = score;
                bestShift = shift;
            }
        };

        //calculate hamming distance for each shift

        //shift >= 0
        for(int shift = 0; shift < subjectLength - minoverlap + 1; ++shift){
            const int overlapsize = std::min(subjectLength - shift, queryLength);

            handle_shift(shift, overlapsize,
                            subjectdata_hi, subjectdata_lo, identity,
                            subjectInts,
                            querydata_hi, querydata_lo, identity);
        }

        //load subject again from memory since it has been modified by calculations with shift >= 0
        std::copy(subject, subject + subjectbytes, subjectdata.begin());

        // shift < 0
        for(int shift = -1; shift >= -queryLength + minoverlap; --shift){
            const int overlapsize = std::min(subjectLength, queryLength + shift);

            handle_shift(shift, overlapsize,
                            querydata_hi, querydata_lo, identity,
                            queryInts,
                            subjectdata_hi, subjectdata_lo, identity);
        }

        AlignmentResult result;
        result.isValid = (bestShift != -queryLength);

        const int queryoverlapbegin_incl = std::max(-bestShift, 0);
        const int queryoverlapend_excl = std::min(queryLength, subjectLength - bestShift);
        const int overlapsize = queryoverlapend_excl - queryoverlapbegin_incl;
        const int opnr = bestScore - totalbases + 2*overlapsize;

        result.score = bestScore;
        result.overlap = overlapsize;
        result.shift = bestShift;
        result.nOps = opnr;

        return result;
    }


    std::vector<AlignmentResult>
    cpu_multi_shifted_hamming_distance_popcount(const char* subject_charptr,
                                int subjectLength,
                                const std::vector<char>& querydata,
                                const std::vector<int>& queryLengths,
                                int max_sequence_bytes,
                                int min_overlap,
                                float maxErrorRate,
                                float min_overlap_ratio) noexcept{

        assert(max_sequence_bytes % 4 == 0);

        if(queryLengths.size() == 0) return {};

        auto getNumBytes = [] (int sequencelength){
            return Sequence2BitHiLo::getNumBytes(sequencelength);
        };

        auto popcount = [](auto i){return __builtin_popcount(i);};

        auto identity = [](auto i){return i;};

        auto hammingDistanceWithShift = [&](int shift, int overlapsize, int max_errors,
                                    unsigned int* shiftptr_hi, unsigned int* shiftptr_lo, auto transfunc1,
                                    int shiftptr_size,
                                    const unsigned int* otherptr_hi, const unsigned int* otherptr_lo,
                                    auto transfunc2){

            const int shiftamount = shift == 0 ? 0 : 1;

            shiftBitArrayLeftBy(shiftptr_hi, shiftptr_size / 2, shiftamount, transfunc1);
            shiftBitArrayLeftBy(shiftptr_lo, shiftptr_size / 2, shiftamount, transfunc1);

            const int score = hammingdistanceHiLo(shiftptr_hi,
                                                shiftptr_lo,
                                                otherptr_hi,
                                                otherptr_lo,
                                                overlapsize,
                                                overlapsize,
                                                max_errors,
                                                transfunc1,
                                                transfunc2,
                                                popcount);

            return score;
        };


        const int nQueries = int(queryLengths.size());

        std::vector<AlignmentResult> results(queryLengths.size());

        const unsigned int* const subject = (const unsigned int*)subject_charptr;
        const int subjectints = getNumBytes(subjectLength) / sizeof(unsigned int);
        const int max_sequence_ints = max_sequence_bytes / sizeof(unsigned int);

        std::vector<unsigned int> shiftbuffer(max_sequence_ints);

        const unsigned int* const subjectBackup_hi = (const unsigned int*)(subject);
        const unsigned int* const subjectBackup_lo = ((const unsigned int*)subject) + subjectints / 2;

        for(int index = 0; index < nQueries; index++){
            const unsigned int* const query = (const unsigned int*)(querydata.data() + max_sequence_bytes * index);
            const int queryLength = queryLengths[index];

            const int queryints = getNumBytes(queryLength) / sizeof(unsigned int);
            const unsigned int* const queryBackup_hi = query;
            const unsigned int* const queryBackup_lo = query + queryints / 2;

            const int totalbases = subjectLength + queryLength;
            const int minoverlap = std::max(min_overlap, int(float(subjectLength) * min_overlap_ratio));


            int bestScore = totalbases; // score is number of mismatches
            int bestShift = -queryLength; // shift of query relative to subject. shift < 0 if query begins before subject

            auto handle_shift = [&](int shift, int overlapsize,
                                        unsigned int* shiftptr_hi, unsigned int* shiftptr_lo, auto transfunc1,
                                        int shiftptr_size,
                                        const unsigned int* otherptr_hi, const unsigned int* otherptr_lo,
                                        auto transfunc2){

                const int max_errors = int(float(overlapsize) * maxErrorRate);

                int score = hammingDistanceWithShift(shift, overlapsize, max_errors,
                                    shiftptr_hi,shiftptr_lo, transfunc1,
                                    shiftptr_size,
                                    otherptr_hi, otherptr_lo, transfunc2);

                score = (score < max_errors ?
                        score + totalbases - 2*overlapsize // non-overlapping regions count as mismatches
                        : std::numeric_limits<int>::max()); // too many errors, discard

                if(score < bestScore){
                    bestScore = score;
                    bestShift = shift;
                }
            };

            std::copy(subject, subject + subjectints, shiftbuffer.begin());
            unsigned int* shiftbuffer_hi = shiftbuffer.data();
            unsigned int* shiftbuffer_lo = shiftbuffer.data() + subjectints / 2;

            for(int shift = 0; shift < subjectLength - minoverlap + 1; ++shift){
                const int overlapsize = std::min(subjectLength - shift, queryLength);
                handle_shift(shift, overlapsize,
                                shiftbuffer_hi, shiftbuffer_lo, identity,
                                subjectints,
                                queryBackup_hi, queryBackup_lo, identity);
            }

            std::copy(query, query + queryints, shiftbuffer.begin());
            shiftbuffer_hi = shiftbuffer.data();
            shiftbuffer_lo = shiftbuffer.data() + queryints / 2;

            for(int shift = -1; shift >= -queryLength + minoverlap; --shift){
                const int overlapsize = std::min(subjectLength, queryLength + shift);

                handle_shift(shift, overlapsize,
                                shiftbuffer_hi, shiftbuffer_lo, identity,
                                queryints,
                                subjectBackup_hi, subjectBackup_lo, identity);

            }

            AlignmentResult& alignmentresult = results[index];
            alignmentresult.isValid = (bestShift != -queryLength);

            const int queryoverlapbegin_incl = std::max(-bestShift, 0);
            const int queryoverlapend_excl = std::min(queryLength, subjectLength - bestShift);
            const int overlapsize = queryoverlapend_excl - queryoverlapbegin_incl;
            const int opnr = bestScore - totalbases + 2*overlapsize;

            alignmentresult.score = bestScore;
            alignmentresult.overlap = overlapsize;
            alignmentresult.shift = bestShift;
            alignmentresult.nOps = opnr;
        }

        return results;
    }


}
}
}
