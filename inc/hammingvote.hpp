#ifndef HAMMING_VOTE_HPP
#define HAMMING_VOTE_HPP

#include "alignment.hpp"

#include <string>

void hamming_vote_global_init();

std::string cpu_hamming_vote(const std::string& subject, 
				std::vector<std::string>& queries, 
				const std::vector<AlignResult>& alignments,
				const std::vector<int>& overlapErrors, 
				const std::vector<int>& overlapSizes,
				const std::string& subjectqualityScores, 
				const std::vector<std::string>& queryqualityScores,
				double maxErrorRate,
				double alpha, 
				double x,
				bool useQScores,
				const std::vector<bool> correctThisQuery,
				bool correctQueries_);


#endif
