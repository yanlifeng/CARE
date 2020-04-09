#include <args.hpp>
#include <hpc_helpers.cuh>
#include <util.hpp>
#include <config.hpp>
#include <readlibraryio.hpp>
#include <minhasher.hpp>
#include <filehelpers.hpp>

#include <iostream>
#include <thread>
#include <string>
#include <stdexcept>

#include <experimental/filesystem>

namespace filesys = std::experimental::filesystem;

namespace care{
namespace args{

    std::vector<std::string> split(const std::string& str, char c){
    	std::vector<std::string> result;

    	std::stringstream ss(str);
    	std::string s;

    	while (std::getline(ss, s, c)) {
    		result.emplace_back(s);
    	}

    	return result;
    }


	template<>
	MinhashOptions to<MinhashOptions>(const cxxopts::ParseResult& pr){
        const int coverage = pr["coverage"].as<float>();
        MinhashOptions result{pr["hashmaps"].as<int>(),
    					      pr["kmerlength"].as<int>(),
                              calculateResultsPerMapThreshold(coverage)};

        return result;
	}


	template<>
	AlignmentOptions to<AlignmentOptions>(const cxxopts::ParseResult& pr){
        AlignmentOptions result;

        return result;
	}

	template<>
	GoodAlignmentProperties to<GoodAlignmentProperties>(const cxxopts::ParseResult& pr){
        GoodAlignmentProperties result{
            pr["minalignmentoverlap"].as<int>(),
            pr["maxmismatchratio"].as<float>(),
            pr["minalignmentoverlapratio"].as<float>(),
        };

        return result;
	}

	template<>
	CorrectionOptions to<CorrectionOptions>(const cxxopts::ParseResult& pr){
        CorrectionOptions result{
            pr["candidateCorrection"].as<bool>(),
			pr["useQualityScores"].as<bool>(),
            pr["coverage"].as<float>(),
            pr["errorfactortuning"].as<float>(),
            pr["coveragefactortuning"].as<float>(),
            pr["kmerlength"].as<int>(),
            pr["batchsize"].as<int>(),
            pr["candidateCorrectionNewColumns"].as<int>(),
        };

        return result;
	}

	template<>
	RuntimeOptions to<RuntimeOptions>(const cxxopts::ParseResult& pr){
        RuntimeOptions result;

		result.threads = pr["threads"].as<int>();
		result.nInserterThreads = std::min(result.threads, (int)std::min(4u, std::thread::hardware_concurrency()));
		result.nCorrectorThreads = std::min(result.threads, (int)std::thread::hardware_concurrency());
        result.showProgress = pr["progress"].as<bool>();

        auto deviceIdsStrings = pr["deviceIds"].as<std::vector<std::string>>();

        for(const auto& s : deviceIdsStrings){
            result.deviceIds.emplace_back(std::stoi(s));
        }

        result.canUseGpu = result.deviceIds.size() > 0;

        return result;
	}

    template<>
	MemoryOptions to<MemoryOptions>(const cxxopts::ParseResult& pr){
        MemoryOptions result;

        auto parseMemoryString = [](const auto& string) -> std::size_t{
            if(string.length() > 0){
                std::size_t factor = 1;
                switch(string.back()){
                    case 'K':{
                        factor = std::size_t(1) << 10; 
                    }break;
                    case 'M':{
                        factor = std::size_t(1) << 20;
                    }break;
                    case 'G':{
                        factor = std::size_t(1) << 30;
                    }break;
                }
                const auto numberString = string.substr(0, string.size()-1);
                return factor * std::stoull(numberString);
            }else{
                return 0;
            }
        };

        if(pr.count("memTotal") > 0){
            const auto memoryTotalLimitString = pr["memTotal"].as<std::string>();
            result.memoryTotalLimit = parseMemoryString(memoryTotalLimitString);
        }else{
            std::size_t availableMemoryInBytes = getAvailableMemoryInKB() * 1024;
            if(availableMemoryInBytes > 2*(std::size_t(1) << 30)){
                availableMemoryInBytes = availableMemoryInBytes - 2*(std::size_t(1) << 30);
            }

            result.memoryTotalLimit = availableMemoryInBytes;
        }

        if(pr.count("memHashtables") > 0){
            const auto memoryForHashtablesString = pr["memHashtables"].as<std::string>();
            result.memoryForHashtables = parseMemoryString(memoryForHashtablesString);
        }else{
            std::size_t availableMemoryInBytes = result.memoryTotalLimit;
            if(availableMemoryInBytes > 1*(std::size_t(1) << 30)){
                availableMemoryInBytes = availableMemoryInBytes - 1*(std::size_t(1) << 30);
            }

            result.memoryForHashtables = availableMemoryInBytes;
        }

        result.memoryForHashtables = std::min(result.memoryForHashtables, result.memoryTotalLimit);
        

        

        return result;
	}

	template<>
	FileOptions to<FileOptions>(const cxxopts::ParseResult& pr){
        FileOptions result;

		result.inputfile = pr["inputfile"].as<std::string>();
		result.outputdirectory = pr["outdir"].as<std::string>();
        result.outputfilename = pr["outfile"].as<std::string>();

        if(result.outputfilename == "")
            result.outputfilename = "corrected_" + filehelpers::getFileName(result.inputfile);

		result.outputfile = result.outputdirectory + "/" + result.outputfilename;

        result.format = getFileFormat(result.inputfile);

		result.nReads = pr["nReads"].as<std::uint64_t>();
        result.minimum_sequence_length = pr["min_length"].as<int>();
        result.maximum_sequence_length = pr["max_length"].as<int>();
        result.save_binary_reads_to = pr["save-preprocessedreads-to"].as<std::string>();
        result.load_binary_reads_from = pr["load-preprocessedreads-from"].as<std::string>();
        result.save_hashtables_to = pr["save-hashtables-to"].as<std::string>();
        result.load_hashtables_from = pr["load-hashtables-from"].as<std::string>();

        if(pr.count("tempdir") > 0){
            result.tempdirectory = pr["tempdir"].as<std::string>();
        }else{
            result.tempdirectory = result.outputdirectory;
        }

        return result;
	}



    template<>
    bool isValid<MinhashOptions>(const MinhashOptions& opt){
        bool valid = true;

        if(opt.maps < 1){
            valid = false;
            std::cout << "Error: Number of hashmaps must be >= 1, is " + std::to_string(opt.maps) << std::endl;
        }

        if(opt.k < 1 || opt.k > max_k<kmer_type>::value){
            valid = false;
            std::cout << "Error: kmer length must be in range [1, " << max_k<kmer_type>::value 
                << "], is " + std::to_string(opt.k) << std::endl;
        }

        return valid;
    }

    template<>
    bool isValid<AlignmentOptions>(const AlignmentOptions& opt){
        bool valid = true;

        return valid;
    }

    template<>
    bool isValid<GoodAlignmentProperties>(const GoodAlignmentProperties& opt){
        bool valid = true;

        if(opt.maxErrorRate < 0.0f || opt.maxErrorRate > 1.0f){
            valid = false;
            std::cout << "Error: maxmismatchratio must be in range [0.0, 1.0], is " + std::to_string(opt.maxErrorRate) << std::endl;
        }

        if(opt.min_overlap < 1){
            valid = false;
            std::cout << "Error: min_overlap must be > 0, is " + std::to_string(opt.min_overlap) << std::endl;
        }

        if(opt.min_overlap_ratio < 0.0f || opt.min_overlap_ratio > 1.0f){
            valid = false;
            std::cout << "Error: min_overlap_ratio must be in range [0.0, 1.0], is "
                        + std::to_string(opt.min_overlap_ratio) << std::endl;
        }

        return valid;
    }

    template<>
    bool isValid<CorrectionOptions>(const CorrectionOptions& opt){
        bool valid = true;

        if(opt.estimatedCoverage <= 0.0f){
            valid = false;
            std::cout << "Error: estimatedCoverage must be > 0.0, is " + std::to_string(opt.estimatedCoverage) << std::endl;
        }

        if(opt.estimatedErrorrate <= 0.0f){
            valid = false;
            std::cout << "Error: estimatedErrorrate must be > 0.0, is " + std::to_string(opt.estimatedErrorrate) << std::endl;
        }

        if(opt.batchsize < 1 /*|| corOpts.batchsize > 16*/){
            valid = false;
            std::cout << "Error: batchsize must be in range [1, ], is " + std::to_string(opt.batchsize) << std::endl;
        }

        return valid;
    }

    template<>
    bool isValid<RuntimeOptions>(const RuntimeOptions& opt){
        bool valid = true;

        if(opt.threads < 1){
            valid = false;
            std::cout << "Error: threads must be > 0, is " + std::to_string(opt.threads) << std::endl;
        }

        // if(opt.threadsForGPUs < 0){
        //     valid = false;
        //     std::cout << "Error: threadsForGPUs must be >= 0, is " + std::to_string(opt.threadsForGPUs) << std::endl;
        // }
        //
        // if(opt.threadsForGPUs > opt.threads){
        //     valid = false;
        //     std::cout << "Error: threadsForGPUs must be <= threads, is " + std::to_string(opt.threadsForGPUs) << std::endl;
        // }

        return valid;
    }

    template<>
    bool isValid<MemoryOptions>(const MemoryOptions& opt){
        bool valid = true;

        return valid;
    }

    template<>
    bool isValid<FileOptions>(const FileOptions& opt){
        bool valid = true;

        {
            std::ifstream is(opt.inputfile);
            if(!(bool)is){
                valid = false;
                std::cout << "Error: cannot find input file " << opt.inputfile << std::endl;
            }
        }

        if(!filesys::exists(opt.tempdirectory)){
            bool created = filesys::create_directories(opt.tempdirectory);
            if(!created){
                valid = false;
                std::cout << "Error: Could not create temp directory" << opt.tempdirectory << std::endl;
            }
        }

        if(!filesys::exists(opt.outputdirectory)){
            bool created = filesys::create_directories(opt.outputdirectory);
            if(!created){
                valid = false;
                std::cout << "Error: Could not create output directory" << opt.outputdirectory << std::endl;
            }
        }

        {
            std::ofstream os(opt.outputfile);
            if(!(bool)os){
                valid = false;
                std::cout << "Error: cannot open output file " << opt.outputfile << std::endl;
            }
        }

        {
            std::ofstream os(opt.tempdirectory+"/tmptest");
            if(!(bool)os){
                valid = false;
                std::cout << "Error: cannot open temporary test file " << opt.tempdirectory+"/tmptest" << std::endl;
            }else{
                filehelpers::removeFile(opt.tempdirectory+"/tmptest");
            }
        }
        
        return valid;
    }

}
}
