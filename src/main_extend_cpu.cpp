#include <version.hpp>
#include <config.hpp>

#include <cxxopts/cxxopts.hpp>
#include <options.hpp>
#include <dispatch_care_extend_cpu.hpp>

#include <threadpool.hpp>

#include <readlibraryio.hpp>

#include <fstream>
#include <iostream>
#include <ios>
#include <string>
#include <omp.h>

#include <experimental/filesystem>

namespace filesys = std::experimental::filesystem;

using namespace care;

void printCommandlineArguments(std::ostream& out, const cxxopts::ParseResult& parseresults){

	const auto args = parseresults.arguments();
	for(const auto& opt : args){
		out << opt.key() << '=' << opt.value() << '\n';
	}
}

bool checkMandatoryArguments(const cxxopts::ParseResult& parseresults){

	const std::vector<std::string> mandatory = {
		"inputfiles", "outdir", "outputfilenames", "coverage",
		"insertsize", "insertsizedev", "pairmode"
	};

	bool success = true;
	for(const auto& opt : mandatory){
		if(parseresults.count(opt) == 0){
			success = false;
			std::cerr << "Mandatory argument " << opt << " is missing.\n";
		}
	}

	return success;
}

template<class T>
std::string tostring(const T& t){
	return std::to_string(t);
}

template<>
std::string tostring(const bool& b){
	return b ? "true" : "false";
}

int main(int argc, char** argv){

	bool help = false;
	bool showVersion = false;

	cxxopts::Options commandLineOptions(argv[0], "CARE-Extender");

	addMandatoryOptions(commandLineOptions);
	addMandatoryOptionsExtend(commandLineOptions);
	addMandatoryOptionsExtendCpu(commandLineOptions);

	addAdditionalOptions(commandLineOptions);
	addAdditionalOptionsExtend(commandLineOptions);
	addAdditionalOptionsExtendCpu(commandLineOptions);

	commandLineOptions.add_options("Additional")			
		("help", "Show this help message", cxxopts::value<bool>(help))
		("version", "Print version", cxxopts::value<bool>(showVersion))
		

	;

	//commandLineOptions.parse_positional({"deviceIds"});

	auto parseresults = commandLineOptions.parse(argc, argv);

	if(showVersion){
		std::cout << "CARE version " << CARE_VERSION_STRING << std::endl;
		std::exit(0);
	}

	if(help) {
		std::cout << commandLineOptions.help({"", "Mandatory", "Additional"}) << std::endl;
		std::exit(0);
	}

	//printCommandlineArguments(std::cerr, parseresults);

	const bool mandatoryPresent = checkMandatoryArguments(parseresults);
	if(!mandatoryPresent){
		std::cout << commandLineOptions.help({"Mandatory"}) << std::endl;
		std::exit(0);
	}

	ProgramOptions programOptions(parseresults);

	programOptions.batchsize = 16;

	if(!programOptions.isValid()) throw std::runtime_error("Invalid program options!");

    programOptions.canUseGpu = false;

	if(programOptions.useQualityScores){
		// const bool fileHasQscores = hasQualityScores(programOptions.inputfile);

		// if(!fileHasQscores){
		// 	std::cerr << "Quality scores have been disabled because no quality scores were found in the input file.\n";
			
		// 	programOptions.useQualityScores = false;
		// }
		
		const bool hasQ = std::all_of(
			programOptions.inputfiles.begin(),
			programOptions.inputfiles.end(),
			[](const auto& s){
				return hasQualityScores(s);
			}
		);

		if(!hasQ){
			std::cerr << "Quality scores have been disabled because there exist reads in an input file without quality scores.\n";
			
			programOptions.useQualityScores = false;
		}

	}

	if(programOptions.pairType == SequencePairType::SingleEnd){
        throw std::runtime_error("single end extension not possible");
    }

	//print all options that will be used
	std::cout << std::boolalpha;
	std::cout << "CARE EXTEND CPU  will be started with the following parameters:\n";

	std::cout << "----------------------------------------\n";


	std::cout << "Alignment absolute required overlap: " << programOptions.min_overlap << "\n";
	std::cout << "Alignment relative required overlap: " << programOptions.min_overlap_ratio << "\n";
	std::cout << "Alignment max relative number of mismatches in overlap: " << programOptions.maxErrorRate << "\n";

	std::cout << "Number of hash tables / hash functions: " << programOptions.numHashFunctions << "\n";
	if(programOptions.autodetectKmerlength){
		std::cout << "K-mer size for hashing: auto\n";
	}else{
		std::cout << "K-mer size for hashing: " << programOptions.kmerlength << "\n";
	}
	
	std::cout << "Exclude ambigious reads: " << programOptions.excludeAmbiguousReads << "\n";
	std::cout << "Use quality scores: " << programOptions.useQualityScores << "\n";
	std::cout << "Estimated dataset coverage: " << programOptions.estimatedCoverage << "\n";
	std::cout << "errorfactortuning: " << programOptions.estimatedErrorrate << "\n";
	std::cout << "coveragefactortuning: " << programOptions.m_coverage << "\n";

	std::cout << "Insert size: " << programOptions.insertSize << "\n";
	std::cout << "Insert size deviation: " << programOptions.insertSizeStddev << "\n";
	std::cout << "Allow extension outside of gap: " << programOptions.allowOutwardExtension << "\n";
	std::cout << "Sort extended reads: " << programOptions.sortedOutput << "\n";
	std::cout << "Output remaining reads: " << programOptions.outputRemainingReads << "\n";

	std::cout << "Threads: " << programOptions.threads << "\n";
	std::cout << "Show progress bar: " << programOptions.showProgress << "\n";

	std::cout << "Maximum memory for hash tables: " << programOptions.memoryForHashtables << "\n";
	std::cout << "Maximum memory total: " << programOptions.memoryTotalLimit << "\n";
	std::cout << "Hashtable load factor: " << programOptions.hashtableLoadfactor << "\n";
	std::cout << "Bits per quality score: " << programOptions.qualityScoreBits << "\n";

	std::cout << "Paired mode: " << to_string(programOptions.pairType) << "\n";
	std::cout << "Output directory: " << programOptions.outputdirectory << "\n";
	std::cout << "Temporary directory: " << programOptions.tempdirectory << "\n";
	std::cout << "Save preprocessed reads to file: " << programOptions.save_binary_reads_to << "\n";
	std::cout << "Load preprocessed reads from file: " << programOptions.load_binary_reads_from << "\n";
	std::cout << "Save hash tables to file: " << programOptions.save_hashtables_to << "\n";
	std::cout << "Load hash tables from file: " << programOptions.load_hashtables_from << "\n";
	std::cout << "Input files: ";
	for(auto& s : programOptions.inputfiles){
		std::cout << s << ' ';
	}
	std::cout << "\n";
	std::cout << "Extended reads output file: " << programOptions.extendedReadsOutputfilename << "\n";
	std::cout << "Output file names: ";
	for(auto& s : programOptions.outputfilenames){
		std::cout << s << ' ';
	}
	std::cout << "\n";
	std::cout << "fixedStddev: " << programOptions.fixedStddev << "\n";
	std::cout << "fixedStepsize: " << programOptions.fixedStepsize << "\n";
	std::cout << "Allow outward extension: " << programOptions.allowOutwardExtension << "\n";
	std::cout << "Sorted output: " << programOptions.sortedOutput << "\n";
	std::cout << "Output remaining reads: " << programOptions.outputRemainingReads << "\n";
	std::cout << "----------------------------------------\n";
	std::cout << std::noboolalpha;

	if(programOptions.pairType == SequencePairType::SingleEnd || programOptions.pairType == SequencePairType::Invalid){
		std::cout << "Only paired-end extension is supported. Abort.\n";
		return 0;
	}


    const int numThreads = programOptions.threads;

	omp_set_num_threads(numThreads);

	care::performExtension(
		programOptions
	);

	return 0;
}
