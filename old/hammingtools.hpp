#ifndef HAMMINGTOOLS_HPP
#define HAMMINGTOOLS_HPP

#include "alignment.hpp"
#include "read.hpp"
#include "batchelem.hpp"
#include "options.hpp"

#include <vector>
#include <chrono>
#include <memory>

namespace care{
namespace hammingtools{

struct SHDdata{

	AlignResultCompact* d_results = nullptr;
	char* d_subjectsdata = nullptr;
	char* d_queriesdata = nullptr;
	int* d_subjectlengths = nullptr;
	int* d_querylengths = nullptr;

	AlignResultCompact* h_results = nullptr;
	char* h_subjectsdata = nullptr;
	char* h_queriesdata = nullptr;
	int* h_subjectlengths = nullptr;
	int* h_querylengths = nullptr;

#ifdef __NVCC__
	std::unique_ptr<cudaStream_t[]> streams;
#endif

	int deviceId = -1;
	size_t sequencepitch = 0;
	int max_sequence_length = 0;
	int max_sequence_bytes = 0;
	int n_subjects = 0;
	int n_queries = 0;
	int max_n_subjects = 0;
	int max_n_queries = 0;

	int batchsize = 1;

	std::chrono::duration<double> resizetime{0};
	std::chrono::duration<double> preprocessingtime{0};
	std::chrono::duration<double> h2dtime{0};
	std::chrono::duration<double> alignmenttime{0};
	std::chrono::duration<double> d2htime{0};
	std::chrono::duration<double> postprocessingtime{0};

	SHDdata(int deviceId_, int batchsize, int maxseqlength, int maxseqbytes);

	void resize(int n_sub, int n_quer);
};

void print_SHDdata(const SHDdata& data);

void cuda_cleanup_SHDdata(SHDdata& data);

void init_once();

void getMultipleAlignments(SHDdata& mybuffers, std::vector<BatchElem>& batch, const GoodAlignmentProperties& props, bool useGpu);


} //end namespace hammingtools
}


#endif