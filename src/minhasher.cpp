#include "../inc/minhasher.hpp"

#include "../inc/binarysequencehelpers.hpp"
#include "../inc/ntHash/nthash.hpp"
#include <stdexcept>
#include <chrono>


Minhasher::Minhasher() : Minhasher(MinhashParameters{1,1})
{
}


Minhasher::Minhasher(const MinhashParameters& parameters) 
		: minparams(parameters), load(0.0), minhashtime(0), maptime(0)
{

}

void Minhasher::init(){

	init(MAX_READ_NUM, 0.80);
}

void Minhasher::init(std::uint64_t nReads, double load_){

	if(nReads > MAX_READ_NUM+1)
		throw std::runtime_error("Minhasher, not enough bits to enumerate all reads");

	load = load_;

	const std::uint64_t capacity = nReads / load;
	std::cout << "minhashmap capacity: " << capacity << ". size per map: " << ((capacity * 8.0)/(1024.0*1024.0*1024.0)) << " GB" << std::endl;

	minhashTables.resize(minparams.maps);

	for (int i = 0; i < minparams.maps; ++i) {
		minhashTables[i].reset(new oa_hash_t { capacity, hash_func {}, prob_func {} });
	}
}


void Minhasher::clear(){
	for (int i = 0; i < minparams.maps; ++i) {
		minhashTables[i]->clear();
	}
}


int Minhasher::insertSequence(const std::string& sequence, const std::uint64_t readnum)
{
	if(readnum > MAX_READ_NUM)
		throw std::runtime_error("Minhasher, too many reads, read number too large");

	// we do not consider reads which are shorter than k
	if(sequence.size() < unsigned(minparams.k))
		return false;

	std::uint64_t bandHashValues[minparams.maps];
	std::fill(bandHashValues, bandHashValues + minparams.maps, 0);

	const unsigned int hvPerMap = 1;
	const unsigned int numberOfHashvalues = minparams.maps * hvPerMap;

	std::uint32_t isForwardStrand[numberOfHashvalues];
	std::fill(isForwardStrand, isForwardStrand + numberOfHashvalues, 0);

	//get hash values
	make_minhash_band_hashes(sequence, bandHashValues, isForwardStrand);	

	// insert
	for (int map = 0; map < minparams.maps; ++map) {
		std::uint64_t key = bandHashValues[map] & hv_bitmask;
		// last bit of value is 1 if the hash value comes from the forward strand
		std::uint64_t value = ((readnum << 1) | isForwardStrand[map]);

		if (!minhashTables[map]->add(key, value)) {
			std::cout << "error adding key to map " << map 
				<< ". key = " << key 
				<< " , readnum = " << readnum << std::endl;
			throw std::runtime_error(("error adding key to map. key " + key));
		}
	}

	return 1;
}

std::vector<std::pair<std::uint64_t, int>> Minhasher::getCandidates(const std::string& sequence) const{

	// we do not consider reads which are shorter than k
	if(sequence.size() < unsigned(minparams.k))
		return {};

	std::uint64_t bandHashValues[minparams.maps];
	std::fill(bandHashValues, bandHashValues + minparams.maps, 0);

	const unsigned int numberOfHashvalues = minparams.maps;
	std::uint32_t isForwardStrand[numberOfHashvalues];
	std::fill(isForwardStrand, isForwardStrand + numberOfHashvalues, 0);

	make_minhash_band_hashes(sequence, bandHashValues, isForwardStrand);

	std::vector<std::pair<std::uint64_t, int>> result;

	std::map<std::uint64_t, int> fwdrevmapping;

	for(int map = 0; map < minparams.maps; ++map) {
		std::uint64_t key = bandHashValues[map] & hv_bitmask;

		std::vector<uint64_t> entries = minhashTables[map]->get(key);
//		std::vector<uint64_t> entries = minhashTables[map]->get_unsafe(key);
		for(const auto x : entries){
			int increment = (x & 1) ? 1 : -1;
			std::uint64_t readnum = (x >> 1);
			fwdrevmapping[readnum] += increment;
		}
	}

	result.insert(result.cend(), fwdrevmapping.cbegin(), fwdrevmapping.cend());

	return result;
}


int Minhasher::make_minhash_band_hashes(const std::string& sequence, std::uint64_t* bandHashValues, std::uint32_t* isForwardStrand) const
{
	int val = minhashfunc(sequence, bandHashValues, isForwardStrand);

	return val;
}

int Minhasher::minhashfunc(const std::string& sequence, std::uint64_t* minhashSignature, std::uint32_t* isForwardStrand) const{
	std::uint64_t fhVal = 0; std::uint64_t rhVal = 0;

	// bitmask for kmer, k_max = 32
	const int kmerbits = (2*unsigned(minparams.k) <= bits_key ? 2*minparams.k : bits_key);

	//const std::uint64_t kmerbitmask = (minparams.k < 32 ? (1ULL << (2 * minparams.k)) - 1 : 1ULL - 2);
	const std::uint64_t kmerbitmask = (kmerbits < 64 ? (1ULL << kmerbits) - 1 : 1ULL - 2);

	const int numberOfHashvalues = minparams.maps;
	std::uint64_t kmerHashValues[numberOfHashvalues];

	std::fill(kmerHashValues, kmerHashValues + numberOfHashvalues, 0);

	std::uint32_t isForward = 0;
	// calc hash values of first canonical kmer
	NTMC64(sequence.c_str(), minparams.k, numberOfHashvalues, minhashSignature, fhVal, rhVal, isForward);

	for (int j = 0; j < numberOfHashvalues; ++j) {
		minhashSignature[j] &= kmerbitmask;
		isForwardStrand[j] = isForward;
	}

	//calc hash values of remaining canonical kmers
	for (size_t i = 0; i < sequence.size() - minparams.k; ++i) {
		NTMC64(fhVal, rhVal, sequence[i], sequence[i + minparams.k], minparams.k, numberOfHashvalues, kmerHashValues, isForward);

		for (int j = 0; j < numberOfHashvalues; ++j) {
			std::uint64_t tmp = kmerHashValues[j] & kmerbitmask;
			if (minhashSignature[j] > tmp){
				minhashSignature[j] = tmp;
				isForwardStrand[j] = isForward;
			}
		}
	}

	return 0;
}

void Minhasher::bandhashfunc(const std::uint64_t* minhashSignature, std::uint64_t* bandHashValues) const{
	// xor the minhash hash values that belong to the same band
	for (int map = 0; map < minparams.maps; map++) {
		bandHashValues[map] ^= minhashSignature[map];
	}
}

	
