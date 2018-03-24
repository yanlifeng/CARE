#include "../inc/errorcorrector.hpp"

#include "../inc/ganja/hpc_helpers.cuh"

#include "../inc/cxxopts/cxxopts.hpp"


#include <iostream>
#include <string>
#include <chrono>
#include <cstdlib>
#include <experimental/filesystem>

namespace filesys = std::experimental::filesystem;

std::string getFileName(std::string filePath)
{
	filesys::path path(filePath);
	return path.filename().string();
}

int main(int argc, const char** argv){

	bool help = false;


	cxxopts::Options options(argv[0], "Perform error correction on a fastq file");

	options.add_options("Group")
		("h", "Show this help message", cxxopts::value<bool>(help))
		("inputfile", "The fastq file to correct", cxxopts::value<std::string>())
		("outdir", "The output directory", cxxopts::value<std::string>()->default_value("."))
		("outfile", "The output file", cxxopts::value<std::string>()->default_value("")->implicit_value(""))
		("hashmaps", "The number of hash maps. Must be greater than 0.", cxxopts::value<int>()->default_value("2")->implicit_value("2"))
		("kmerlength", "The kmer length for minhashing. Must be greater than 0.", cxxopts::value<int>()->default_value("16")->implicit_value("16"))
		("insertthreads", "Number of threads to build database. Must be greater than 0.", cxxopts::value<int>()->default_value("1")->implicit_value("1"))
		("correctorthreads", "Number of threads to correct reads. Must be greater than 0.", cxxopts::value<int>()->default_value("1")->implicit_value("1"))
		("base", "Graph parameter for cutoff (alpha*pow(base,edge))", cxxopts::value<double>()->default_value("1.1")->implicit_value("1.1"))
		("alpha", "Graph parameter for cutoff (alpha*pow(base,edge))", cxxopts::value<double>()->default_value("1.0")->implicit_value("1.0"))
		("batchsize", "This mainly affects the GPU alignment since the alignments of batchsize reads to their candidates is done in parallel.Must be greater than 0.",
				 cxxopts::value<int>()->default_value("5"))
		("useQualityScores", "If set, quality scores (if any) are considered during read correction",
				 cxxopts::value<bool>()->default_value("false")->implicit_value("true"))

		("matchscore", "Score for match during alignment.", cxxopts::value<int>()->default_value("1")->implicit_value("1"))
		("subscore", "Score for substitution during alignment.", cxxopts::value<int>()->default_value("-1")->implicit_value("-1"))
		("insertscore", "Score for insertion during alignment.", cxxopts::value<int>()->default_value("-100")->implicit_value("-100"))
		("deletionscore", "Score for deletion during alignment.", cxxopts::value<int>()->default_value("-100")->implicit_value("-100"))

		("maxmismatchratio", "Overlap between query and candidate must contain at most maxmismatchratio * overlapsize mismatches",
					cxxopts::value<double>()->default_value("0.2")->implicit_value("0.2"))
		("minalignmentoverlap", "Overlap between query and candidate must be at least this long", cxxopts::value<int>()->default_value("35")->implicit_value("35"))
		("minalignmentoverlapratio", "Overlap between query and candidate must be at least as long as minalignmentoverlapratio * querylength",
					cxxopts::value<double>()->default_value("0.35")->implicit_value("0.35"))
		("fileformat", "Format of input file. Allowed values: {fasta, fastq}",
					cxxopts::value<std::string>()->default_value("fastq")->implicit_value("fastq"))

		("coverage", "estimated coverage of input file",
					cxxopts::value<double>()->default_value("20.0")->implicit_value("20.0"))
		("errorrate", "estimated error rate of input file",
					cxxopts::value<double>()->default_value("0.03")->implicit_value("0.03"))
		("m_coverage", "m",
					cxxopts::value<double>()->default_value("0.6")->implicit_value("0.6"))

	;

	auto parseresults = options.parse(argc, argv);

	if(help){
	      	std::cout << options.help({"", "Group"}) << std::endl;
		exit(0);
	}

	care::ErrorCorrector corrector(parseresults);

    std::string inputfile = parseresults["inputfile"].as<std::string>();
    std::string fileformat = parseresults["fileformat"].as<std::string>();
    std::string outputdirectory = parseresults["outdir"].as<std::string>();
    std::string outputfile;
    if(parseresults["outfile"].as<std::string>() == ""){
        outputfile = outputdirectory + "/corrected_" + getFileName(inputfile);
    }else{
        outputfile = outputdirectory + "/" + parseresults["outfile"].as<std::string>();
    }
    filesys::create_directories(outputdirectory);

	corrector.correct(inputfile, fileformat, outputfile);


	return 0;
}
